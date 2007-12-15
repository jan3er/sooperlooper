/*
** Copyright (C) 2004 Jesse Chappell <jesse@essej.net>
**  
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**  
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**  
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**  
*/
#include <iostream>

#include <cmath>
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>
#include <cerrno>

#include <vector>

#include <pbd/xml++.h>

#include "version.h"
#include "engine.hpp"
#include "looper.hpp"
#include "control_osc.hpp"
#include "midi_bind.hpp"
#include "midi_bridge.hpp"
#include "utils.hpp"

using namespace SooperLooper;
using namespace std;
using namespace PBD;
using namespace SigC;

#define MAX_EVENTS 1024
#define MAX_SYNC_EVENTS 1024


Engine::Engine ()
{
	_ok = false;
	_driver = 0;
	_osc = 0;
	_event_generator = 0;
	_event_queue = 0;
	_def_channel_cnt = 2;
	_def_loop_secs = 200;
	_tempo = 110.0;
	_eighth_cycle = 16.0f;
	_sync_source = NoSync;
	_tempo_counter = 0;
	_tempo_frames = 0;
	_quarter_counter = 0.0;
	_quarter_note_frames = 0.0;
	_midi_ticks = 0;
	_midi_loop_tick = 12;
	_midi_bridge = 0;
	_learn_done = false;
	_received_done = false;
	_target_common_dry = 0.0f;
	_curr_common_dry = 0.0f;
	_target_common_wet = 1.0f;
	_curr_common_wet = 1.0f;
	_target_input_gain = 1.0f;
	_curr_input_gain = 1.0f;
	_common_input_peak = 0.0f;
	_common_output_peak = 0.0f;
	_auto_disable_latency = true;
	_loading = false;
	_use_temp_input = true; // all the time for now
	_ignore_quit = false;
	_selected_loop = -1; // all
	
	_running_frames = 0;
	_last_tempo_frame = 0;
	_tempo_changed = false;
	_beat_occurred = false;
	_conns_changed = false;
	
	_loop_manage_to_rt_queue = 0;
	_loop_manage_to_main_queue = 0;

	_solo_down_stamp = 1 << 31;
	
	pthread_cond_init (&_event_cond, NULL);

	reset_avg_tempo();
}

bool Engine::initialize(AudioDriver * driver, int buschans, int port, string pingurl)
{
	char tmpstr[100];
	port_id_t tmpport;

	_driver = driver;
	_driver->set_engine(this);
	
	if (!_driver->initialize()) {
		cerr << "cannot connect to audio driver" << endl;
		return false;
	}

	_buffersize = _driver->get_buffersize();

	_common_input_buffers.clear();
	_common_outputs.clear();
	_common_inputs.clear();
	
	// create common io ports
	for (int i=0; i < buschans; ++i) 
	{
		snprintf(tmpstr, sizeof(tmpstr), "common_in_%d", i+1);
		if (_driver->create_input_port (tmpstr, tmpport)) {
			_common_inputs.push_back (tmpport);
		}

		// create temp input buffer
		sample_t * inbuf = new float[driver->get_buffersize()];
		memset(inbuf, 0, sizeof(float) * driver->get_buffersize());
		_temp_input_buffers.push_back(inbuf);

		_common_input_buffers.push_back(0); // fill to correct size
		
		snprintf(tmpstr, sizeof(tmpstr), "common_out_%d", i+1);
		if (_driver->create_output_port (tmpstr, tmpport)) {
			_common_outputs.push_back (tmpport);
		}
		
		// create temp output buffer
		sample_t * outbuf = new float[driver->get_buffersize()];
		memset(outbuf, 0, sizeof(float) * driver->get_buffersize());
		_temp_output_buffers.push_back(outbuf);
		
		_common_output_buffers.push_back(0); // fill to correct size
	}


	_event_generator = new EventGenerator(_driver->get_samplerate());
	_event_queue = new RingBuffer<Event> (MAX_EVENTS);
	_midi_event_queue = new RingBuffer<Event> (MAX_EVENTS);
	_sync_queue = new RingBuffer<Event> (MAX_SYNC_EVENTS);
	_nonrt_update_event_queue = new RingBuffer<Event> (MAX_SYNC_EVENTS);

	_nonrt_event_queue = new RingBuffer<EventNonRT *> (MAX_EVENTS);

	_loop_manage_to_rt_queue = new RingBuffer<LoopManageEvent> (16);
	_loop_manage_to_main_queue = new RingBuffer<LoopManageEvent> (16);

	// reserve space in instance vectors to try to be RT safe
	_instances.reserve(128);
	_rt_instances.reserve(128);
	
	_internal_sync_buf = new float[driver->get_buffersize()];
	memset(_internal_sync_buf, 0, sizeof(float) * driver->get_buffersize());

	_falloff_per_sample = 30.0f / driver->get_samplerate(); // 30db per second falloff

	_longpress_frames = (nframes_t) lrint (driver->get_samplerate() * 1.0);

	calculate_tempo_frames();
	
	_osc = new ControlOSC(this, port);

	if (!_osc->is_ok()) {
		return false;
	}

	_driver->ConnectionsChanged.connect(slot(*this, &Engine::connections_changed));
	
	_ok = true;

	return true;
}


void
Engine::cleanup()
{
	if (_osc) {
		delete _osc;
		_osc = 0;
	}

	if (_event_queue) {
		delete _event_queue;
		_event_queue = 0;
	}

	if (_midi_event_queue) {
		delete _midi_event_queue;
		_midi_event_queue = 0;
	}
	
	if (_sync_queue) {
		delete _sync_queue;
		_sync_queue = 0;
	}

	if (_nonrt_event_queue) {
		delete _nonrt_event_queue;
		_nonrt_event_queue = 0;
	}

	if (_nonrt_update_event_queue) {
		delete _nonrt_update_event_queue;
		_nonrt_update_event_queue = 0;
	}
	
	if (_event_generator) {
		delete _event_generator;
		_event_generator = 0;
	}

	if (_internal_sync_buf) {
		delete [] _internal_sync_buf;
		_internal_sync_buf = 0;
	}

	if (_loop_manage_to_rt_queue) {
		delete _loop_manage_to_rt_queue;
		_loop_manage_to_rt_queue = 0;
	}
	if (_loop_manage_to_main_queue) {
		delete _loop_manage_to_main_queue;
		_loop_manage_to_main_queue = 0;
	}

	// delete temp common input buffers
	for (vector<sample_t *>::iterator iter = _temp_input_buffers.begin(); iter != _temp_input_buffers.end(); ++iter) 
	{
		sample_t * inbuf = *iter;
		if (inbuf) {
			delete [] inbuf;
		}
	}
	_temp_input_buffers.clear();
	
	// delete temp common input buffers
	for (vector<sample_t *>::iterator iter = _temp_output_buffers.begin(); iter != _temp_output_buffers.end(); ++iter) 
	{
		sample_t * inbuf = *iter;
		if (inbuf) {
			delete [] inbuf;
		}
	}
	_temp_output_buffers.clear();
	
	// safe to do this, we assume all RT activity has stopped here
    for (Instances::iterator iter = _instances.begin(); iter != _instances.end(); ++iter) {
		delete *iter;
	}
	_instances.clear();
	_rt_instances.clear();

	_driver = 0;
	_ok = false;
	
}

Engine::~Engine ()
{
	if (_driver) {
		cleanup ();
	}
}

void Engine::set_midi_bridge (MidiBridge * bridge)
{
	_midi_bridge = bridge;
	if (_midi_bridge) {
		_midi_bridge->BindingLearned.connect(slot(*this, &Engine::binding_learned));
		_midi_bridge->NextMidiReceived.connect(slot(*this, &Engine::next_midi_received));

		_midi_bridge->MidiCommandEvent.connect (slot(*this, &Engine::push_midi_command_event));
		_midi_bridge->MidiControlEvent.connect (slot(*this, &Engine::push_midi_control_event));
		_midi_bridge->MidiSyncEvent.connect (slot(*this, &Engine::push_sync_event));
	}
}

void Engine::quit(bool force)
{
	if (!force && _ignore_quit) {
		return;
	}
	
	_ok = false;

	LockMonitor mon(_event_loop_lock, __LINE__, __FILE__);
	pthread_cond_signal (&_event_cond);
}

bool
Engine::get_common_input (unsigned int chan, port_id_t & port)
{
	if (chan < _common_inputs.size()) {
		port = _common_inputs[chan];
		return true;
	}

	return false;
}

bool
Engine::get_common_output (unsigned int chan, port_id_t & port)
{
	if (chan < _common_outputs.size()) {
		port = _common_outputs[chan];
		return true;
	}

	return false;
}

sample_t *
Engine::get_common_input_buffer (unsigned int chan)
{
	if (chan < _common_inputs.size()) {
		if (_use_temp_input) {
			return _temp_input_buffers[chan];
		}
		else {
			// assumed this is in process cycle and already cached
			return _common_input_buffers[chan];
		}
	}
	return 0;
}

sample_t *
Engine::get_common_output_buffer (unsigned int chan)
{
	if (chan < _common_outputs.size()) {
		if (_use_temp_input) {
			return _temp_output_buffers[chan];
		}
		else {
			// assumed this is in process cycle and already cached
			return _common_output_buffers[chan];
		}
		//return _driver->get_output_port_buffer (_common_outputs[chan], _buffersize);
	}
	return 0;
}


void
Engine::buffersize_changed (nframes_t nframes)
{
	if (_buffersize != nframes)
	{
		
		// called from the audio thread callback
		size_t m = 0;
		for (Instances::iterator i = _rt_instances.begin(); i != _rt_instances.end(); ++i, ++m)
		{
			(*i)->set_buffer_size(nframes);
		}

		// resize temp inbuf and sync buf
		for (int n=0; n < 2; ++n) 
		{
			delete [] _temp_input_buffers[n];
			delete [] _temp_output_buffers[n];
			// create temp input buffer
			sample_t * inbuf = new float[nframes];
			memset(inbuf, 0, sizeof(float) * nframes);
			_temp_input_buffers[n] = inbuf;
			// temp output
			sample_t * outbuf = new float[nframes];
			memset(outbuf, 0, sizeof(float) * nframes);
			_temp_output_buffers[n] = outbuf;
		}

		delete [] _internal_sync_buf;
		_internal_sync_buf = new float[nframes];
		memset(_internal_sync_buf, 0, sizeof(float) * nframes);

		_buffersize = nframes;
	}
}

void
Engine::connections_changed()
{
	// called from the audio thread callback
	size_t m = 0;
	for (Instances::iterator i = _rt_instances.begin(); i != _rt_instances.end(); ++i, ++m)
	{
		(*i)->recompute_latencies ();
	}

	// cause certain values to be updated
	_conns_changed = true;
}

void 
Engine::fill_common_outs(nframes_t nframes)
{
	sample_t * outbuf, * real_outbuf;
	sample_t * inbuf;
	float dry_delta = flush_to_zero(_target_common_dry - _curr_common_dry) / max((nframes_t) 1, (nframes - 1));
	float wet_delta = flush_to_zero(_target_common_wet - _curr_common_wet) / max((nframes_t) 1, (nframes - 1));
	float currdry = 1.0f, currwet = 1.0f;
	float inpeak = _common_input_peak;
	float outpeak = _common_output_peak;
	
	// do fixed peak meter falloff
	inpeak = flush_to_zero (f_clamp (DB_CO (CO_DB(inpeak) - nframes * _falloff_per_sample), 0.0f, 20.0f));
	outpeak = flush_to_zero (f_clamp (DB_CO (CO_DB(outpeak) - nframes * _falloff_per_sample), 0.0f, 20.0f));
	
	// assume ins and out count the same
	for (size_t i=0; i < _common_outputs.size(); ++i) 
	{
		currdry = _curr_common_dry;
		currwet = _curr_common_wet;
		inbuf = _common_input_buffers[i]; // use true common input (not gain attenuated)
		real_outbuf = _common_output_buffers[i];
		if (_use_temp_input) {
			outbuf = _temp_output_buffers[i]; // use temp outputs 
		}
		else {
			outbuf = _common_output_buffers[i];
		}
			//_driver->get_output_port_buffer (_common_outputs[i], _driver->get_buffersize());

		for (nframes_t n = 0; n < nframes; ++n) {
			currdry += dry_delta;
			currwet += wet_delta;

			inpeak = f_max (inpeak, fabsf(inbuf[n]));

			real_outbuf[n] = flush_to_zero ((outbuf[n] * currwet) + (inbuf[n] * currdry));

			// outpeak is taken post dry/wet mix for true output metering
			outpeak = f_max (outpeak, fabsf(real_outbuf[n]));
			
		}
	}

	_curr_common_dry = flush_to_zero (currdry);
	_curr_common_wet = flush_to_zero (currwet);
	if (dry_delta <= 0.00003f) {
		_curr_common_dry = _target_common_dry;
	}
	if (wet_delta <= 0.00003f) {
		_curr_common_wet = _target_common_wet;
	}

	_common_output_peak = outpeak;
	_common_input_peak = inpeak;
}

void 
Engine::prepare_buffers(nframes_t nframes)
{
	sample_t * outbuf, *inbuf;
	float ing_delta = flush_to_zero(_target_input_gain - _curr_input_gain) / max((nframes_t) 1, (nframes - 1));
	float curr_ing = _curr_input_gain;
	
	for (size_t i=0; i < _common_outputs.size(); ++i) 
	{
		sample_t * real_inbuf = _driver->get_input_port_buffer (_common_inputs[i], _driver->get_buffersize());
		outbuf = _driver->get_output_port_buffer (_common_outputs[i], _driver->get_buffersize());
		
		//if (real_inbuf != outbuf) {
		//	memset (outbuf, 0, nframes * sizeof(sample_t));
		//}

		memset (_temp_output_buffers[i], 0, nframes * sizeof(sample_t));		
		
		// attenuate common inputs

		_common_output_buffers[i] = outbuf;
		_common_input_buffers[i] = real_inbuf; // for use later in the process cycle
		inbuf = _temp_input_buffers[i];
		curr_ing = _curr_input_gain;
		
		for (nframes_t n = 0; n < nframes; ++n) {
			curr_ing += ing_delta;
			inbuf[n] = real_inbuf[n] * curr_ing;
		}
		
	}

	_curr_input_gain = flush_to_zero (curr_ing);
	if (ing_delta <= 0.00003f) {
		// force to == target
		_curr_input_gain = _target_input_gain;
	}
	
}


bool
Engine::add_loop (unsigned int chans, float loopsecs, bool discrete)
{
	int n;
	
	n = _instances.size();
	
	Looper * instance;
	
	instance = new Looper (_driver, (unsigned int) n, chans, loopsecs, discrete);
	
	if (!(*instance)()) {
		cerr << "can't create a new loop!\n";
		delete instance;
		return false;
	}
	
	// set some initial controls
	float quantize_value = QUANT_OFF;
	float round_value = 0.0f;
	float relative_sync = 0.0f;
	
	if (!_instances.empty()) {
		quantize_value = _instances[0]->get_control_value (Event::Quantize);
		round_value = _instances[0]->get_control_value (Event::Round);
		relative_sync = _instances[0]->get_control_value (Event::RelativeSync);
	}
	
	instance->set_port (Quantize, quantize_value);
	instance->set_port (Round, round_value);
	instance->set_port (RelativeSync, relative_sync);
	instance->set_port (EighthPerCycleLoop, _eighth_cycle);
	instance->set_port (TempoInput, _tempo);
	
	return add_loop (instance);
}

bool
Engine::add_loop (Looper * instance)
{
	_instances.push_back (instance);
	
	bool val = _auto_disable_latency && _target_common_dry > 0.0f;
	instance->set_disable_latency_compensation (val);


	update_sync_source();

	LoopAdded (instance->get_index(), !_loading); // emit

	// now we push the added loop to the RT thread
	LoopManageEvent lmev(LoopManageEvent::AddLoop, instance);
	push_loop_manage_to_rt (lmev);
	
	return true;
}


bool
Engine::remove_loop (Looper * looper)
{

	if (_instances.back() == looper) {
		_instances.pop_back();
	}
	else {
		// less efficient
		Instances::iterator iter = find (_instances.begin(), _instances.end(), looper);
		if (iter != _instances.end()) {
			_instances.erase(iter);
		}
	}
	
	delete looper;

	if (!_loading) {
		LoopRemoved(); // emit
	}
	
	update_sync_source();

	if (_selected_loop >= (int) _instances.size()) {
		_selected_loop = 0;
	}

	
	return true;
}


std::string
Engine::get_osc_url (bool udp)
{
	if (_osc && _osc->is_ok()) {
		if (udp) {
			return _osc->get_server_url();
		}
		else {
			return _osc->get_unix_server_url();
		}
	}

	return "";
}

int
Engine::get_osc_port ()
{
	if (_osc && _osc->is_ok()) {
		return _osc->get_server_port();
	}

	return 0;
}


static inline Event * next_rt_event (RingBuffer<Event>::rw_vector & vec, size_t & pos,
				     RingBuffer<Event>::rw_vector & midivec, size_t & midipos)
{
	Event * e1 = 0;
	Event * e2 = 0;
	
	if (pos < vec.len[0]) {
		e1 = &vec.buf[0][pos];
	}
	else if (pos < (vec.len[0] + vec.len[1])) {
		e1 = &vec.buf[1][pos - vec.len[0]];
	}

	if (midipos < midivec.len[0]) {
		e2 = &midivec.buf[0][midipos];
	}
	else if (midipos < (midivec.len[0] + midivec.len[1])) {
		e2 = &midivec.buf[1][midipos - midivec.len[0]];
	}

	// pick the earliest fragpos

	if (e1 && e2) {
		if (e1->FragmentPos() < e2->FragmentPos()) {
			++pos;
			return e1;
		}
		else {
			++midipos;
			return e2;
		}
	}
	else if (e1) {
		++pos;
		return e1;
	}
	else if (e2) {
		++midipos;
		return e2;
	}

	return 0;
}
	
void Engine::process_rt_loop_manage_events ()
{
	// pull off all loop management events from the main thread
	LoopManageEvent * lmevt;
	
	while (is_ok() && _loop_manage_to_rt_queue->read_space() > 0)
	{
		RingBuffer<LoopManageEvent>::rw_vector vec;
		_loop_manage_to_rt_queue->get_read_vector(&vec);
		lmevt = vec.buf[0];
		
		// we must remove/add this loop from our copy of the vector
		if (lmevt->etype == LoopManageEvent::RemoveLoop)
		{
			if (_rt_instances.back() == lmevt->looper) {
				_rt_instances.pop_back();
			}
			else {
				// less efficient
				Instances::iterator iter = find (_rt_instances.begin(), _rt_instances.end(), lmevt->looper);
				if (iter != _rt_instances.end()) {
					_rt_instances.erase(iter);
				}
			}
			
			// signal mainloop it is safe to delete
			push_loop_manage_to_main (*lmevt);
			
		}
		else if (lmevt->etype == LoopManageEvent::AddLoop)
		{
			_rt_instances.push_back (lmevt->looper);
			lmevt->looper->recompute_latencies();
		}
		
		_loop_manage_to_rt_queue->increment_read_ptr(1);
	}
	
}


int
Engine::process (nframes_t nframes)
{
	//TentativeLockMonitor lm (_instance_lock, __LINE__, __FILE__);
	//if (!lm.locked()) {
	//	return 0;
	//}

	// process events
	//cerr << "process"  << endl;

	Event * evt;
	RingBuffer<Event>::rw_vector vec;
	RingBuffer<Event>::rw_vector midivec;

	// get available events
	_event_queue->get_read_vector (&vec);
	_midi_event_queue->get_read_vector (&midivec);
		
	// update event generator
	_event_generator->updateFragmentTime (nframes);

	// process loop instance rt events
	process_rt_loop_manage_events();
	
	// update internal sync
	calculate_tempo_frames ();
	generate_sync (0, nframes);
	
	// clear common output buffers
	prepare_buffers (nframes);

	nframes_t usedframes = 0;
	nframes_t doframes;
	size_t num = vec.len[0] + midivec.len[0];
	size_t n = 0;
	size_t midi_n = 0;
	int fragpos;
	int m, syncm;
	
	if (num > 0) {

		evt = next_rt_event (vec, n, midivec, midi_n);
		
		while (evt)
		{ 
			fragpos = (nframes_t) evt->FragmentPos();

			if (fragpos < (int) usedframes || fragpos >= (int) nframes) {
				// bad fragment pos
#ifdef DEBUG
				cerr << "BAD FRAGMENT POS: " << fragpos << endl;
#endif
				// do immediately
				fragpos = usedframes;
			}

			doframes = fragpos - usedframes;

			// handle special global RT events
			if (evt->Instance == -2 
			    || evt->Command == Event::SOLO
			    || evt->Command == Event::SOLO_NEXT ||  evt->Command == Event::SOLO_PREV
			    || evt->Command == Event::RECORD_SOLO_NEXT ||  evt->Command == Event::RECORD_SOLO_PREV 
			    || evt->Command == Event::RECORD_SOLO)
			{
				do_global_rt_event (evt, usedframes + doframes, nframes - (usedframes + doframes));
			}

			m = 0;
			syncm = -1;

			
			if ((int)_sync_source > 0 && (int)_sync_source <= (int)_rt_instances.size()) {
				// we need to run the sync source loop first
				syncm = (int) _sync_source - 1;
				_rt_instances[syncm]->run (usedframes, doframes);
				
				if (evt->Instance == -1 || evt->Instance == syncm ||
				    (evt->Instance == -3 && (_selected_loop == syncm || _selected_loop == -1))) {
					_rt_instances[syncm]->do_event (evt);
				}
			}

			
			for (Instances::iterator i = _rt_instances.begin(); i != _rt_instances.end(); ++i, ++m)
			{
				if (syncm == m) continue; // skip if we already ran it
				
				// run for the time before this event

				(*i)->run (usedframes, doframes);
					
				// process event
				if (evt->Instance == -1 || evt->Instance == m || 
				    (evt->Instance == -3 && (_selected_loop == m || _selected_loop == -1))) {
					(*i)->do_event (evt);
				}
			}

			usedframes += doframes;

			evt = next_rt_event (vec, n, midivec, midi_n);
		}

		// advance events
		_event_queue->increment_read_ptr (vec.len[0] + vec.len[1]);
		_midi_event_queue->increment_read_ptr (midivec.len[0] + midivec.len[1]);


		m = 0;
		syncm = -1;

		if ((int)_sync_source > 0 && (int)_sync_source <= (int) _rt_instances.size()) {
			// we need to run the sync source loop first
			syncm = (int) _sync_source - 1;
			_rt_instances[syncm]->run (usedframes, nframes - usedframes);
		}
		
		
		// run the rest of the frames
		for (Instances::iterator i = _rt_instances.begin(); i != _rt_instances.end(); ++i ,++m) {
			if (syncm == m) continue;

			(*i)->run (usedframes, nframes - usedframes);
		}

	}
	else {
		// no events

		int m = 0;
		int syncm = -1;
		
		if ((int)_sync_source > 0 && (int) _sync_source <= (int)_rt_instances.size()) {
			// we need to run the sync source loop first
			syncm = (int) _sync_source - 1;
			_rt_instances[syncm]->run (0, nframes);
		}

		for (Instances::iterator i = _rt_instances.begin(); i != _rt_instances.end(); ++i, ++m) {
			if (syncm == m) continue;
			(*i)->run (0, nframes);
		}

	}

	// scales output and mixes common dry
	fill_common_outs (nframes);

	_running_frames += nframes;
	
	return 0;
}

void
Engine::do_global_rt_event (Event * ev, nframes_t offset, nframes_t nframes)
{
	if (ev->Control == Event::TapTempo) {
		nframes_t thisframe = _running_frames + offset;
		if (thisframe > _last_tempo_frame) {
			double ntempo = ((double) _driver->get_samplerate() / (double)(thisframe - _last_tempo_frame)) * 60.0f; 

			//cerr << "TAP: new tempo: " << _tempo  << "  off: " << offset << endl;
			ntempo = avg_tempo(ntempo);
			
			set_tempo(ntempo, true);
			calculate_tempo_frames ();

			if (_sync_source == InternalTempoSync) {
				generate_sync (offset, nframes);
			}

			_tempo_changed = true;
			// wake up mainloop safely
			//TentativeLockMonitor mon(_event_loop_lock,  __LINE__, __FILE__);
			//if (mon.locked()) {
			pthread_cond_signal (&_event_cond);
			//}
			
		}
		
		_last_tempo_frame = thisframe;
	}
	else if (ev->Control == Event::DryLevel)
	{
		if (ev->Value != _target_common_dry) {
			
			// if > 0 and auto_disable_latency is enabled, disable compensation for each loop
			//   otherwise, enable compensation

			if ((_target_common_dry == 0.0f) || (ev->Value == 0.0f)) {
				bool val = _auto_disable_latency && ev->Value > 0.0f;

				for (Instances::iterator i = _rt_instances.begin(); i != _rt_instances.end(); ++i) {
					(*i)->set_disable_latency_compensation (val);
				}
				_conns_changed = true;
			}

			_target_common_dry = ev->Value;
		}
		
	}
	else if (ev->Command == Event::SOLO || ev->Command == Event::SOLO_NEXT ||  ev->Command == Event::SOLO_PREV
		 ||  ev->Command == Event::RECORD_SOLO_NEXT ||  ev->Command == Event::RECORD_SOLO_PREV || ev->Command == Event::RECORD_SOLO)
	{
		// notify all loops they are being soloed or not (this acts as a toggle)
		int target_instance = (ev->Instance == -3) ? _selected_loop : ev->Instance;

		if (ev->Type == Event::type_cmd_down || ev->Type == Event::type_cmd_hit || ev->Type == Event::type_cmd_upforce
		    || (ev->Type == Event::type_cmd_up && _running_frames > (_solo_down_stamp + _longpress_frames)))
		{	
			if (ev->Command == Event::SOLO_NEXT ||  ev->Command == Event::RECORD_SOLO_NEXT) {
				// increment selected
				_selected_loop = (_selected_loop + 1) % _rt_instances.size();
				_sel_loop_changed = true;
				target_instance = _selected_loop;
			}
			else if (ev->Command == Event::SOLO_PREV ||  ev->Command == Event::RECORD_SOLO_PREV) {
				// decrement selected
				_selected_loop = _selected_loop > 0 ? _selected_loop - 1 : _rt_instances.size() - 1;
				_sel_loop_changed = true;
				target_instance = _selected_loop;
			}
		}


		if (target_instance >= 0 && target_instance < (int) _rt_instances.size()) 
		{
			if (ev->Type == Event::type_cmd_down || ev->Type == Event::type_cmd_hit || ev->Type == Event::type_cmd_upforce
			    || (ev->Type == Event::type_cmd_up && _running_frames > (_solo_down_stamp + _longpress_frames)))
			{
				bool target_solo_state = _rt_instances[target_instance]->is_soloed();
				
				for (Instances::iterator i = _rt_instances.begin(); i != _rt_instances.end(); ++i) 
				{
					(*i)->set_soloed (target_instance, !target_solo_state);
				}

				if (ev->Type == Event::type_cmd_down) {
					_solo_down_stamp = _running_frames;
				} else {
					_solo_down_stamp = 1 << 31;
				}
			}
		}
		
		if (ev->Command == Event::RECORD_SOLO_NEXT ||  ev->Command == Event::RECORD_SOLO_PREV || ev->Command == Event::RECORD_SOLO)
		{
			// change the instance to the target we soloed, and the command to record
			ev->Instance = target_instance;
			ev->Command = Event::RECORD;
		}
		else {
			// change the event's instance so it isn't used later in the process call
			ev->Instance = -100;
		}
	}
	else if (ev->Control == Event::WetLevel)
	{
		_target_common_wet = ev->Value;
	}
	else if (ev->Control == Event::InputGain)
	{
		_target_input_gain = ev->Value;
	}
	else if (ev->Control == Event::AutoDisableLatency)
	{
		_auto_disable_latency = ev->Value;
		bool val = _auto_disable_latency && _target_common_dry > 0.0f;
	
		for (Instances::iterator i = _rt_instances.begin(); i != _rt_instances.end(); ++i) {
			(*i)->set_disable_latency_compensation (val);
		}
		_conns_changed = true;
	}
	else if (ev->Control == Event::SelectedLoopNum)
	{
		_selected_loop = (int) ev->Value;
		_sel_loop_changed = true;
	}
	else if (ev->Control == Event::SelectAllLoops)
	{
		_selected_loop = (int) -1;
		_sel_loop_changed = true;
	}
	else if (ev->Control == Event::SelectPrevLoop)
	{
		_selected_loop = _selected_loop > 0 ? _selected_loop - 1 : _rt_instances.size() - 1;
		_sel_loop_changed = true;
	}
	else if (ev->Control == Event::SelectNextLoop)
	{
		_selected_loop = (_selected_loop + 1) % _rt_instances.size();
		_sel_loop_changed = true;
	}
	else if (ev->Control == Event::SyncTo)
	{
		if ((int) ev->Value > (int) FIRST_SYNC_SOURCE
		    && ev->Value <= _rt_instances.size())
		{
			_sync_source = (SyncSourceType) (int) roundf(ev->Value);
			update_sync_source();
		}
	}	
	else if (ev->Control == Event::Tempo) {
		if (ev->Value > 0.0f) {
			set_tempo((double) ev->Value, false);
		}
	}
	else if (ev->Control == Event::EighthPerCycle) {
		if (ev->Value > 0.0f) {
			_eighth_cycle = ev->Value;
			
			// update all loops
			for (unsigned int n=0; n < _rt_instances.size(); ++n) {
				_rt_instances[n]->set_port(EighthPerCycleLoop, _eighth_cycle);
			}
		}
		calculate_midi_tick(true);
	}

}
	
bool Engine::push_loop_manage_to_rt (LoopManageEvent & lme)
{
	if (_loop_manage_to_rt_queue->write (&lme, 1) != 1) {
#ifdef DEBUG
		cerr << "event loopmanage_to_main full, dropping event" << endl;
#endif
		return false;
	}
	return true;
}

bool Engine::push_loop_manage_to_main (LoopManageEvent & lme)
{
	if (_loop_manage_to_main_queue->write (&lme, 1) != 1) {
#ifdef DEBUG
		cerr << "event loopmanage_to_main full, dropping event" << endl;
#endif
		return false;
	}
	return true;
}

bool
Engine::push_command_event (Event::type_t type, Event::command_t cmd, int8_t instance)
{
	bool ret = do_push_command_event (_event_queue, type, cmd, instance);

	
	// this is a known race condition, if the osc thread is changing controls
	// simultaneously.  it's just an update :)
//	do_push_command_event (_nonrt_update_event_queue, type, cmd,instance);

	// wakeup nonrt loop... this lock should really not block... but still
//	TentativeLockMonitor mon(_event_loop_lock,  __LINE__, __FILE__);
//	pthread_cond_signal (&_event_cond);

	return ret;
}

void
Engine::push_midi_command_event (Event::type_t type, Event::command_t cmd, int8_t instance, long framepos)
{
	do_push_command_event (_midi_event_queue, type, cmd, instance, framepos);

	// this is a known race condition, if the osc thread is changing controls
	// simultaneously.  it's just an update :)
//	do_push_command_event (_nonrt_update_event_queue, type, cmd,instance);

	// wakeup nonrt loop... this lock should really not block... but still
//	TentativeLockMonitor mon(_event_loop_lock,  __LINE__, __FILE__);
//	pthread_cond_signal (&_event_cond);
}


bool
Engine::do_push_command_event (RingBuffer<Event> * evqueue, Event::type_t type, Event::command_t cmd, int8_t instance, long framepos)
{
	// todo support more than one simulataneous pusher safely
	RingBuffer<Event>::rw_vector vec;

	evqueue->get_write_vector (&vec);

	if (vec.len[0] == 0) {
#ifdef DEBUG
		cerr << "event queue full, dropping event" << endl;
#endif
		return false;
	}
	
	Event * evt = vec.buf[0];
	*evt = get_event_generator().createEvent(framepos);

	evt->Type = type;
	evt->Command = cmd;
	evt->Instance = instance;

	evqueue->increment_write_ptr (1);

	return true;
}


bool
Engine::do_push_control_event (RingBuffer<Event> * evqueue, Event::type_t type, Event::control_t ctrl, float val, int8_t instance, long framepos, int src)
{
	// todo support more than one simulataneous pusher safely

	RingBuffer<Event>::rw_vector vec;

	evqueue->get_write_vector (&vec);

	if (vec.len[0] == 0) {
#ifdef DEBUG
		cerr << "event queue full, dropping event" << endl;
#endif
		return false;
	}
	
	Event * evt = vec.buf[0];
	*evt = get_event_generator().createEvent(framepos);

	evt->Type = type;
	evt->Control = ctrl;
	evt->Value = val;
	evt->Instance = instance;
	evt->source = src;

	evqueue->increment_write_ptr (1);
	
	return true;
}

void
Engine::push_control_event (Event::type_t type, Event::control_t ctrl, float val, int8_t instance, int src)
{
	do_push_control_event (_event_queue, type, ctrl, val, instance);

	// this is a known race condition, if the midi thread is changing controls
	// simultaneously.  it's just an update :)
	do_push_control_event (_nonrt_update_event_queue, type, ctrl, val, instance, src);

	// wakeup nonrt loop... this lock should really not block... but still
	TentativeLockMonitor mon(_event_loop_lock,  __LINE__, __FILE__);
	pthread_cond_signal (&_event_cond);
	
}

void
Engine::push_midi_control_event (Event::type_t type, Event::control_t ctrl, float val, int8_t instance, long framepos)
{
	do_push_control_event (_midi_event_queue, type, ctrl, val, instance, framepos);

	// this is a known race condition, if the osc thread is changing controls
	// simultaneously.  it's just an update :)
	do_push_control_event (_nonrt_update_event_queue, type, ctrl, val, instance);

	// wakeup nonrt loop... this lock should really not block... but still
	TentativeLockMonitor mon(_event_loop_lock,  __LINE__, __FILE__);
	pthread_cond_signal (&_event_cond);
}

void
Engine::push_sync_event (Event::control_t ctrl, long framepos)
{
	// todo support more than one simulataneous pusher safely

	if (_sync_source != MidiClockSync) {
		return;
	}
	
	RingBuffer<Event>::rw_vector vec;

	_sync_queue->get_write_vector (&vec);

	if (vec.len[0] == 0) {
#ifdef DEBUG
		cerr << "sync event queue full, dropping event" << endl;
#endif
		return;
	}
	
	Event * evt = vec.buf[0];
	*evt = get_event_generator().createEvent(framepos);

	evt->Type = Event::type_sync;
	evt->Control = ctrl;

	_sync_queue->increment_write_ptr (1);
	
	return;
}


float
Engine::get_control_value (Event::control_t ctrl, int8_t instance)
{
	// not really anymore, this is only called from the nonrt work thread
	// that does the allocating of instances
	
	if (instance >= 0 && instance < (int) _instances.size()) {

		return _instances[instance]->get_control_value (ctrl);
	}
	else if (instance == -2) {
		if (ctrl == Event::InPeakMeter) {
			return _common_input_peak;
		}
		else if (ctrl == Event::OutPeakMeter) {
			return _common_output_peak;
		}
		else if (ctrl == Event::DryLevel) {
			return _curr_common_dry;
		}
		else if (ctrl == Event::WetLevel) {
			return _curr_common_wet;
		}
		else if (ctrl == Event::InputGain) {
			return _curr_input_gain;
		}
	}

	return 0.0f;
}


bool
Engine::push_nonrt_event (EventNonRT * event)
{

	_nonrt_event_queue->write(&event, 1);
	
	LockMonitor mon(_event_loop_lock,  __LINE__, __FILE__);
	pthread_cond_signal (&_event_cond);

	return true;
}

	
void
Engine::binding_learned(MidiBindInfo info)
{
	_learn_done = true;
	_learninfo = info;
	
	LockMonitor mon(_event_loop_lock,  __LINE__, __FILE__);
	pthread_cond_signal (&_event_cond);
}

void
Engine::next_midi_received(MidiBindInfo info)
{
	_received_done = true;
	_learninfo = info;
	
	LockMonitor mon(_event_loop_lock,  __LINE__, __FILE__);
	pthread_cond_signal (&_event_cond);
}


void
Engine::mainloop()
{
	struct timespec timeout;
	struct timeval now = {0, 0};
	struct timeval timeoutv = {0, 0};
	int  wait_ret = 0;
	
	EventNonRT * event;
	Event  * evt;
	LoopManageEvent * lmevt;
	
	// non-rt event processing loop

	while (is_ok())
	{
		// pull off all loop management events from the rt thread
		while (is_ok() && _loop_manage_to_main_queue->read_space() > 0)
		{
			RingBuffer<LoopManageEvent>::rw_vector vec;
			_loop_manage_to_main_queue->get_read_vector(&vec);
			lmevt = vec.buf[0];

			// we must finish the removal
			if (lmevt->etype == LoopManageEvent::RemoveLoop) {
				remove_loop (lmevt->looper);
			}
			
			_loop_manage_to_main_queue->increment_read_ptr(1);
		}
		
		// pull off all events from nonrt ringbuffer
		while (is_ok() && _nonrt_event_queue->read_space() > 0 && _nonrt_event_queue->read(&event, 1) == 1)
		{
			process_nonrt_event (event);
			delete event;
		}

		// now pull off special update events
		while (is_ok() && _nonrt_update_event_queue->read_space() > 0)
		{
			RingBuffer<Event>::rw_vector vec;
			_nonrt_update_event_queue->get_read_vector(&vec);
			evt = vec.buf[0];

			if (evt->Control == Event::SaveLoop) {
				cerr << "got save: " << (int) evt->Instance <<  endl;
				// save with no filename will autogenerate a unique name
				for (unsigned int n=0; n < _instances.size(); ++n) {
					if (evt->Instance < 0 || evt->Instance == (int)n) {
						_instances[n]->save_loop ();
					}
				}
			}
			else if (evt->Type == Event::type_control_change) {
				ConfigUpdateEvent cuev (ConfigUpdateEvent::Send, evt->Instance, evt->Control, "", "", evt->Value);
				cuev.source = evt->source;
				_osc->finish_update_event (cuev);
			}
			else if (evt->Type == Event::type_cmd_down || evt->Type == Event::type_cmd_hit) {
				//cerr << "got command: " << evt->Command << endl;
				     
				ConfigUpdateEvent cuev (ConfigUpdateEvent::SendCmd, evt->Instance, evt->Command, "", "");
				cuev.source = evt->source;
				_osc->finish_update_event (cuev);
			}
			
			_nonrt_update_event_queue->increment_read_ptr(1);
		}
		
		if (!is_ok()) break;

		// handle special requests from the audio thread
		// this is a hack for now
		if (_tempo_changed)
		{
			ConfigUpdateEvent cu_event(ConfigUpdateEvent::Send, -2, Event::Tempo, "", "", (float) _tempo);
			_osc->finish_update_event (cu_event);
			_tempo_changed = false;
		}

		if (_sel_loop_changed)
		{
			ConfigUpdateEvent cu_event(ConfigUpdateEvent::Send, -2, Event::SelectedLoopNum, "", "", (float) _selected_loop);
			_osc->finish_update_event (cu_event);
			_sel_loop_changed = false;
		}

		if (_beat_occurred)
		{
			//cerr << "beat occurred" << endl;
			// just use some unique number
			ConfigUpdateEvent cu_event(ConfigUpdateEvent::Send, -2, Event::TapTempo, "", "", (float) _running_frames);
			_osc->finish_update_event (cu_event);
			
			_beat_occurred = false;
		}

		if (_conns_changed)
		{
			// send latency updates
			for (unsigned int n=0; n < _instances.size(); ++n) {
				ConfigUpdateEvent cu_event(ConfigUpdateEvent::Send, n, Event::OutputLatency,
							   "", "", (float) _instances[n]->get_control_value(Event::OutputLatency));
				_osc->finish_update_event (cu_event);

				cu_event.control = Event::InputLatency;
				cu_event.value   =  _instances[n]->get_control_value(Event::InputLatency);
				_osc->finish_update_event (cu_event);
			}
			_conns_changed = false;
		}
		
		// handle learning done from the midi thread
		if (_learn_done) {
			LockMonitor lm (_midi_bridge->bindings_lock(), __LINE__, __FILE__);
			_midi_bridge->bindings().add_binding (_learninfo, _learn_event.options == "exclusive");

			_learn_event.bind_str = _learninfo.serialize();
			_osc->finish_midi_binding_event (_learn_event);
			
			_learn_done = false;
		}
		else if (_received_done) {
			_learn_event.bind_str = _learninfo.serialize();
			_osc->finish_midi_binding_event (_learn_event);
			_received_done = false;
		}

		gettimeofday(&now, NULL);

		// if now is >= then the last timeout target, we should update
		if (wait_ret == ETIMEDOUT || timercmp (&now, &timeoutv, >=)) {
			//cerr << "timed out, sending updates" << endl;
			_osc->send_auto_updates();

			// wake up every 100 ms for servicing auto-update parameters
			// TODO: make it more flexible
			const long up_interval = 100000; // 100 ms

			timeout.tv_sec = now.tv_sec;
			timeout.tv_nsec = (now.tv_usec + up_interval) * 1000;
			if (timeout.tv_nsec > 1000000000) {
				timeout.tv_sec += 1;
				timeout.tv_nsec = (timeout.tv_nsec % 1000000000);
			}
			timeoutv.tv_sec = timeout.tv_sec;
			timeoutv.tv_usec = timeout.tv_nsec / 1000;
		}
		
		// sleep on condition
		{
			LockMonitor mon(_event_loop_lock, __LINE__, __FILE__);
			wait_ret = pthread_cond_timedwait (&_event_cond, _event_loop_lock.mutex(), &timeout);
		}
	}

}

bool
Engine::process_nonrt_event (EventNonRT * event)
{
	ConfigUpdateEvent * cu_event;
	GetParamEvent *     gp_event;
	ConfigLoopEvent *   cl_event;
	PingEvent *         ping_event;
	RegisterConfigEvent * rc_event;
	LoopFileEvent      * lf_event;
	GlobalGetEvent     * gg_event;
	GlobalSetEvent     * gs_event;
	MidiBindingEvent   * mb_event;
	SessionEvent       * sess_event;
	
	if ((gp_event = dynamic_cast<GetParamEvent*> (event)) != 0)
	{
		gp_event->ret_value = get_control_value (gp_event->control, gp_event->instance);
		_osc->finish_get_event (*gp_event);
	}
	else if ((gg_event = dynamic_cast<GlobalGetEvent*> (event)) != 0)
	{
		if (gg_event->param == "dry") {
			gg_event->ret_value = (float) _curr_common_dry;
		}
		else if (gg_event->param == "wet") {
			gg_event->ret_value = (float) _curr_common_wet;
		}
		else if (gg_event->param == "input_gain") {
			gg_event->ret_value = (float) _curr_input_gain;
		}
		else if (gg_event->param == "auto_disable_latency") {
			gg_event->ret_value =  (_auto_disable_latency) ? 1.0f: 0.0f;
		}
		else if (gg_event->param == "selected_loop_num") {
			gg_event->ret_value = (float) _selected_loop;
		}
		else if (gg_event->param == "sync_source") {
			gg_event->ret_value = (float) _sync_source;
		}
		else if (gg_event->param == "tempo") {
			gg_event->ret_value = (float) _tempo;
		}
		else if (gg_event->param == "eighth_per_cycle") {
			gg_event->ret_value = _eighth_cycle;
		}
		
		_osc->finish_global_get_event (*gg_event);
	}
	else if ((gs_event = dynamic_cast<GlobalSetEvent*> (event)) != 0)
	{
		if (gs_event->param == "dry") {
			_target_common_dry = gs_event->value;
		}
		else if (gs_event->param == "wet") {
			_target_common_wet = gs_event->value;
		}
		else if (gs_event->param == "input_gain") {
			_target_input_gain = gs_event->value;
		}
		else if (gs_event->param == "auto_disable_latency") {
			_auto_disable_latency = gs_event->value;
			//cerr << "NEED TO setting disable_compensation " << endl;
		}
		else if (gs_event->param == "selected_loop_num") {
			_selected_loop = (int) gs_event->value;
		}
		else if (gs_event->param == "sync_source") {
			if ((int) gs_event->value > (int) FIRST_SYNC_SOURCE
			    && gs_event->value <= _instances.size())
			{
				_sync_source = (SyncSourceType) (int) roundf(gs_event->value);
				update_sync_source();
			}
		}
		else if (gs_event->param == "tempo") {
			    if (gs_event->value > 0.0f) {
				    set_tempo((double) gs_event->value, true);
			    }
		}
		else if (gs_event->param == "eighth_per_cycle") {
			if (gs_event->value > 0.0f) {
				_eighth_cycle = gs_event->value;

				// update all loops
				for (unsigned int n=0; n < _instances.size(); ++n) {
					_instances[n]->set_port(EighthPerCycleLoop, _eighth_cycle);
				}
			}
			calculate_midi_tick();
		}

	}
	else if ((cu_event = dynamic_cast<ConfigUpdateEvent*> (event)) != 0)
	{
		_osc->finish_update_event (*cu_event);
	}
	else if ((cl_event = dynamic_cast<ConfigLoopEvent*> (event)) != 0)
	{
		if (cl_event->type == ConfigLoopEvent::Add) {
			// todo: use secs
			if (cl_event->channels == 0) {
				cl_event->channels = _def_channel_cnt;
			}
			if (cl_event->secs == 0.0f) {
				cl_event->secs = _def_loop_secs;
			}
			
			add_loop (cl_event->channels, cl_event->secs, cl_event->discrete);
		}
		else if (cl_event->type == ConfigLoopEvent::Remove)
		{
			if (cl_event->index == -1) {
				cl_event->index = _instances.size() - 1;
			}

			// post a new loop remove event to the rt thread
			// we do the real cleanup when it tells us to
			if (cl_event->index >= 0 && cl_event->index < (int) _instances.size())
			{
				LoopManageEvent lmev (LoopManageEvent::RemoveLoop, _instances[cl_event->index]);
				push_loop_manage_to_rt (lmev);
			}
			
			_osc->finish_loop_config_event (*cl_event);
		}
	}
	else if ((mb_event = dynamic_cast<MidiBindingEvent*> (event)) != 0  && _midi_bridge)
	{
		if (mb_event->type == MidiBindingEvent::Add) {
			MidiBindInfo info;
			if (info.unserialize (mb_event->bind_str)) {
				bool exclus = (mb_event->options.find("exclusive") != string::npos);
				LockMonitor lm (_midi_bridge->bindings_lock(), __LINE__, __FILE__);
				_midi_bridge->bindings().add_binding (info, exclus);
			}
		}
		else if (mb_event->type == MidiBindingEvent::Learn)
		{
			MidiBindInfo info;
			if (info.unserialize (mb_event->bind_str)) {
				bool exclus = (mb_event->options.find("exclusive") != string::npos);
				_learn_done = false;
				_learninfo = info;
				_learn_event = *mb_event;

				_midi_bridge->start_learn (info, exclus);
			}
		}
		else if (mb_event->type == MidiBindingEvent::CancelLearn) {
			_learn_done = false;
			_midi_bridge->cancel_learn();
			_osc->finish_midi_binding_event(*mb_event);
		}
		else if (mb_event->type == MidiBindingEvent::CancelGetNext) {
			//cerr << "gcancel get next" << endl;
			_received_done = false;
			_midi_bridge->cancel_get_next();
			_osc->finish_midi_binding_event(*mb_event);
		}
		else if (mb_event->type == MidiBindingEvent::Remove)
		{
			MidiBindInfo info;
			if (info.unserialize (mb_event->bind_str)) {
				LockMonitor lm (_midi_bridge->bindings_lock(), __LINE__, __FILE__);
				_midi_bridge->bindings().remove_binding (info);
			}
		}
		else if (mb_event->type == MidiBindingEvent::GetAll)
		{
			LockMonitor lm (_midi_bridge->bindings_lock(), __LINE__, __FILE__);
			_osc->send_all_midi_bindings (&_midi_bridge->bindings(), mb_event->ret_url, mb_event->ret_path);
		}
		else if (mb_event->type == MidiBindingEvent::Clear)
		{
			LockMonitor lm (_midi_bridge->bindings_lock(), __LINE__, __FILE__);
			_midi_bridge->bindings().clear_bindings();
		}
		else if (mb_event->type == MidiBindingEvent::Load)
		{
			LockMonitor lm (_midi_bridge->bindings_lock(), __LINE__, __FILE__);
			_midi_bridge->bindings().load_bindings (mb_event->bind_str, mb_event->options == "add" ? true: false);
		}
		else if (mb_event->type == MidiBindingEvent::Save)
		{
			LockMonitor lm (_midi_bridge->bindings_lock(), __LINE__, __FILE__);
			_midi_bridge->bindings().save_bindings (mb_event->bind_str);
		}
		else if (mb_event->type == MidiBindingEvent::GetNextMidi)
		{
			//cerr << "get next" << endl;
			_received_done = false;
			_learn_event = *mb_event;
			_midi_bridge->start_get_next();
		}
	}
	else if ((ping_event = dynamic_cast<PingEvent*> (event)) != 0)
	{
		_osc->send_pingack(true, ping_event->ret_url, ping_event->ret_path);
	}
	else if ((rc_event = dynamic_cast<RegisterConfigEvent*> (event)) != 0)
	{
		_osc->finish_register_event (*rc_event);
	}
	else if ((lf_event = dynamic_cast<LoopFileEvent*> (event)) != 0)
	{
		for (unsigned int n=0; n < _instances.size(); ++n) {
			if (lf_event->instance == -1 || lf_event->instance == (int)n) {
				if (lf_event->type == LoopFileEvent::Load) {
					if (!_instances[n]->load_loop (lf_event->filename)) {
						_osc->send_error(lf_event->ret_url, lf_event->ret_path, "Loop Load Failed");
					}
				}
				else {
					if (!_instances[n]->save_loop (lf_event->filename)) {
						_osc->send_error(lf_event->ret_url, lf_event->ret_path, "Loop Save Failed");
					}
				}
			}
		}
	}
	else if ((sess_event = dynamic_cast<SessionEvent*> (event)) != 0)
	{
		if (sess_event->type == SessionEvent::Load) {
			if (!load_session (sess_event->filename)) {
				_osc->send_error(sess_event->ret_url, sess_event->ret_path, "Session Load Failed");
			}
		}
		else {
			if (!save_session (sess_event->filename)) {
				_osc->send_error(sess_event->ret_url, sess_event->ret_path, "Session Save Failed");
			}
		}
	}
	
	return true;
}


void Engine::update_sync_source ()
{
	sample_t * sync_buf = _internal_sync_buf;

	// if sync_source > 0, then get the source from instance
	if (_sync_source == JackSync) {
		sync_buf = _internal_sync_buf;
	}
	else if (_sync_source == MidiClockSync) {
		sync_buf = _internal_sync_buf;
	}
	else if (_sync_source == InternalTempoSync) {
		sync_buf = _internal_sync_buf;
	}
	else if (_sync_source == BrotherSync) {
		sync_buf = _internal_sync_buf;
	}
	else if (_sync_source > 0 && (int)_sync_source <= (int) _instances.size()) {
		sync_buf = _instances[(int)_sync_source - 1]->get_sync_out_buf();
		// cerr << "using sync from " << _sync_source -1 << endl;
	}
	
	for (Instances::iterator i = _instances.begin(); i != _instances.end(); ++i)
	{
		(*i)->use_sync_buf (sync_buf);
	}

	_quarter_counter = 0;

	set_tempo(_tempo, false);
}


void
Engine::set_tempo (double tempo, bool rt)
{
	_tempo = tempo;
	_quarter_counter = 0;
	_tempo_counter = 0;

	if (_sync_source == NoSync) {
		// we set tempo to zero as far as the loop is concerned
		tempo = 0.0;
	}
	
	// update all loops
	if (rt) {
		for (Instances::iterator i = _rt_instances.begin(); i != _rt_instances.end(); ++i)
		{
			(*i)->set_port(TempoInput, tempo);
		}
	}
	else {
		for (Instances::iterator i = _instances.begin(); i != _instances.end(); ++i)
		{
			(*i)->set_port(TempoInput, tempo);
		}
	}
}

void
Engine::calculate_tempo_frames ()
{
	float quantize_value = (float) QUANT_8TH;
		
	if (!_rt_instances.empty()) {
		quantize_value = _rt_instances[0]->get_control_value (Event::Quantize);
	}
	
	if (_sync_source == InternalTempoSync || _sync_source == JackSync)
	{
		if (_tempo > 0.0) {
			if (quantize_value == QUANT_8TH) {
				// calculate number of samples per eighth-note (assuming 2 8ths per beat)
				// samples / 8th = samplerate * (1 / tempo) * 60/2; 
				_tempo_frames = _driver->get_samplerate() * (1.0/_tempo) * 30.0;
			}
			else if (quantize_value == QUANT_CYCLE) {
				// calculate number of samples per cycle given the current eighths per cycle
				// samples / 8th = samplerate * (1 / tempo) * 60/2; 
				// samples / cycle = samples / 8th  *  eighth_per_cycle
				_tempo_frames = _driver->get_samplerate() * (1.0/_tempo) * 30.0 * _eighth_cycle;
			}
			else {
				_tempo_frames = 0; // ???
			}
		}
		else {
			_tempo_frames = 0;
		}

		
		// cerr << "tempo frames is " << _tempo_frames << endl;
	}
	else if (_sync_source == MidiClockSync) {
		calculate_midi_tick (true);
	}

	if (_tempo > 0.0) {
		_quarter_note_frames = _driver->get_samplerate() * (1.0/_tempo) * 60;
	}
	else {
		_quarter_note_frames = 0.0;
	}

}


// Calculate the number of ticks to sync loops to
void
Engine::calculate_midi_tick (bool rt)
{
	float quantize_value = (float) QUANT_8TH;
	if (rt) {
		if (!_rt_instances.empty())
			quantize_value = _rt_instances[0]->get_control_value (Event::Quantize);
	}
	else {
		if (!_instances.empty())
			quantize_value = _instances[0]->get_control_value (Event::Quantize);
	}
	
	if (quantize_value == QUANT_8TH)
		_midi_loop_tick = 12; // 12 ticks per 8th note
	else if (quantize_value == QUANT_CYCLE)
		_midi_loop_tick = static_cast<unsigned int>(_eighth_cycle * 12);
	else
		_midi_loop_tick = 24;  // ..this can't happen, but quarter note is safe
}


int
Engine::generate_sync (nframes_t offset, nframes_t nframes)
{
	nframes_t npos = offset;
	int hit_at = -1;
	
	if (_sync_source == InternalTempoSync) {

		if (_tempo_frames == 0.0) {
			// just calculate quarter note beats for update
			double qcurr = _quarter_counter;
			qcurr += (double) nframes;
			
			if (qcurr >= _quarter_note_frames) {
				// inaccurate
				hit_at = (int) npos;
				qcurr = ((qcurr - _quarter_note_frames) - truncf(qcurr - _quarter_note_frames)) + 1.0;
			}

			_quarter_counter = qcurr;

			// no real sync here
			for (nframes_t n=offset; n < nframes; ++n) {
				_internal_sync_buf[n]  = 1.0;
			}

		}
		else {
			double curr = _tempo_counter;
			double qcurr = _quarter_counter;
			
			while (npos < nframes) {
				
				while (curr < _tempo_frames && qcurr < _quarter_note_frames && npos < nframes) {
					_internal_sync_buf[npos++] = 0.0f;
					curr += 1.0;
					qcurr += 1.0;
				}
				
				if (qcurr >= _quarter_note_frames) {
					hit_at = (int) npos;
					qcurr = ((qcurr - _quarter_note_frames) - truncf(qcurr - _quarter_note_frames)) + 1.0;
				}
				
				if (curr >= _tempo_frames) {
					// cerr << "tempo hit" << endl;
					_internal_sync_buf[npos++] = 1.0f;
					// reset curr counter
					curr = ((curr - _tempo_frames) - truncf(curr - _tempo_frames)) + 1.0;
				}
			}
			
			_tempo_counter = curr;
			_quarter_counter = qcurr;
			//cerr << "tempo counter is now: " << _tempo_counter << endl;
		}
	}
	else if (_sync_source == MidiClockSync) {

		RingBuffer<Event>::rw_vector vec;
		Event *evt;

		// get available events
		_sync_queue->get_read_vector (&vec);
		
		nframes_t usedframes = 0;
		nframes_t doframes;
		size_t num = vec.len[0];
		size_t n = 0;
		size_t vecn = 0;
		nframes_t fragpos;
		

		
		if (num > 0) {
			
			while (n < num)
			{ 
				evt = vec.buf[vecn] + n;
				fragpos = (nframes_t) evt->FragmentPos();
				
				++n;
				// to avoid code copying
				if (n == num) {
					if (vecn == 0) {
						++vecn;
						n = 0;
						num = vec.len[1];
					}
				}
				
				if (fragpos < usedframes || fragpos >= nframes) {
					// bad fragment pos
#ifdef DEBUG
					cerr << "BAD SYNC FRAGMENT POS: " << fragpos << endl;
#endif
					continue;
				}
				
				doframes = fragpos - usedframes;
				
				// handle special global RT events
				if (evt->Control == Event::MidiTick) {
					_midi_ticks++;
				}
				else if (evt->Control == Event::MidiStart) {
					_midi_ticks = 0;
					//cerr << "got start at " << fragpos << endl;
					//calculate_midi_tick();
				}
				else if (evt->Control == Event::MidiStop) {
					// stop playing?
					//cerr << "got stop at " << fragpos << endl;
				}

				if ((_midi_ticks % 24) == 0) {
					// every quarter note
					hit_at = (int) usedframes;
					double tcount = _quarter_counter + (double) usedframes;

					// calc new tempo
					double ntempo = (_driver->get_samplerate() * 60.0 / tcount);
					ntempo = avg_tempo(ntempo);

					if (ntempo != _tempo) {
						//cerr << "new tempo is: " << ntempo << "   tcount = " << tcount << "  used: " << usedframes << endl;
						set_tempo(ntempo, true);
						_tempo_changed = true;
						// wake up mainloop safely
						pthread_cond_signal (&_event_cond);
					}

					_quarter_counter = - ((double)usedframes);
				}
				
				if ((_midi_ticks % _midi_loop_tick) == 0) {
					// cerr << "GOT SYNC TICK at " << fragpos << endl;

					// zero sync before this event
					memset (&(_internal_sync_buf[usedframes]), 0, doframes * sizeof(float));

					// mark it high
					_internal_sync_buf[fragpos] = 1.0f;

					doframes += 1;
					
					//_midi_ticks = 0;
				}
				
				usedframes += doframes;
			}
			
			// advance events
			_sync_queue->increment_read_ptr (vec.len[0] + vec.len[1]);

			// zero the rest
			memset (&(_internal_sync_buf[usedframes]), 0, (nframes - usedframes) * sizeof(float));
			
			_quarter_counter += (double) nframes;
		}
		else {
			// no sync events... all zero
			memset (_internal_sync_buf, 0, nframes * sizeof(float));
			_quarter_counter += (double) nframes;
		}

	}
	else if (_sync_source == NoSync) {
		for (nframes_t n=offset; n < nframes; ++n) {
			_internal_sync_buf[n]  = 1.0;
		}
		//memset (_internal_sync_buf, 0, nframes * sizeof(float));

	}
	else if (_sync_source == JackSync) {

		for (nframes_t n=offset; n < nframes; ++n) {
			_internal_sync_buf[n]  = 0.0;
		}
				
		TransportInfo info;
		if (_driver->get_transport_info(info)) {

			
			if (_tempo != info.bpm) {
				set_tempo(info.bpm, true);
				calculate_tempo_frames ();
				_tempo_changed = true;
				// wake up mainloop safely
				pthread_cond_signal (&_event_cond);
			}

			if (_tempo_frames > 0.0 && info.state == TransportInfo::ROLLING) {
				nframes_t thisval = (nframes_t) lrint (fmod ((double) (info.framepos + offset), _tempo_frames));
				nframes_t nextval = (nframes_t) lrint (fmod ((double) (info.framepos + offset + nframes), _tempo_frames));
				nframes_t diff = lrint(_tempo_frames - thisval);
				diff = (thisval == 0) ? 0 : diff;
				
				//cerr << "pos: " << info.framepos << "  tempoframes: " << _tempo_frames << "  this: " << thisval << "  next: " << nextval << endl;
				
				if ((thisval == 0 || nextval <= thisval) && diff < nframes) {
					//cerr << "got tempo frame in this cycle: diff: " << diff << endl;
					_internal_sync_buf[offset + diff]  = 1.0;
				}
			}

			if (_quarter_note_frames > 0.0 && info.state == TransportInfo::ROLLING) {
				nframes_t thisval = (nframes_t) lrint (fmod ((double) (info.framepos + offset), _quarter_note_frames));
				nframes_t nextval = (nframes_t) lrint (fmod ((double) (info.framepos + offset + nframes), _quarter_note_frames));
				nframes_t diff = lrint(_quarter_note_frames - thisval);
				diff = (thisval == 0) ? 0 : diff;

				//cerr << "pos: " << info.framepos << "  qframes: " << _quarter_note_frames << "  this: " << thisval << "  next: " << nextval << endl;
				
				if ((thisval == 0 || nextval <= thisval) && diff < nframes) {
					//cerr << "got quarter frame in this cycle: diff: " << diff << endl;
					hit_at = (int) diff;
				}
			}
		}
	}
	else if ((int)_sync_source > 0 && (size_t)_sync_source <= _rt_instances.size()) {
		// a loop
		for (nframes_t n=offset; n < nframes; ++n) {
			_internal_sync_buf[n]  = 1.0;
		}

		if (_rt_instances[_sync_source-1]->get_control_value(Event::State) != LooperStateRecording) {
			// calc new tempo
			nframes_t cycleframes = (nframes_t) (_rt_instances[_sync_source-1]->get_control_value(Event::CycleLength) * _driver->get_samplerate());
			double ntempo = 0.0;
			if (cycleframes > 0) {
				ntempo = (_driver->get_samplerate() * 30.0 * _eighth_cycle / cycleframes);
			}
			
			if (ntempo != _tempo) {
				//cerr << "new tempo is: " << ntempo << endl;

				set_tempo(ntempo, true);

				calculate_tempo_frames ();
				_tempo_changed = true;
				// wake up mainloop safely
				pthread_cond_signal (&_event_cond);
			}
			
			// just calculate quarter note beats for update
			if (_quarter_note_frames > 0.0) {
				nframes_t currpos  = (nframes_t) (_rt_instances[_sync_source-1]->get_control_value(Event::LoopPosition) * _driver->get_samplerate());
				nframes_t loopframes = (nframes_t) (_rt_instances[_sync_source-1]->get_control_value(Event::LoopLength) * _driver->get_samplerate());
				if (loopframes > 0) {
					nframes_t testval = (((currpos + nframes) % loopframes) % (nframes_t)_quarter_note_frames);
					
					if (testval <= nframes || testval == 0) {
						// inaccurate
						//cerr << "quarter hit" << endl;
						hit_at = (int) 0;
					}
				}
			}
		}
		
	}
		
	if (hit_at >= 0 && _tempo < 400.0) {
		_beat_occurred = true;
		// wake up mainloop safely
		//TentativeLockMonitor mon(_event_loop_lock,  __LINE__, __FILE__);
		//if (mon.locked()) {
		pthread_cond_signal (&_event_cond);
	}
	
	return hit_at;
}


bool
Engine::load_session (std::string fname, string * readstr)
{
	LocaleGuard lg ("POSIX");
	XMLTree sessiondoc;
	XMLNodeList looper_kids;
	const XMLProperty* prop;

	if (readstr) {
		sessiondoc.read_buffer(*readstr);
	}
	else {
		sessiondoc.read(fname);
	}
	
	if (!sessiondoc.initialized()) {
		fprintf (stderr, "Error loading session at %s!\n", fname.c_str()); 
		return false;
	}

	XMLNode * root_node = sessiondoc.root();
	if (!root_node || root_node->name() != "SLSession") {
		fprintf (stderr, "Preset root node not found in %s!\n", fname.c_str()); 
		return false;
	}

	XMLNode * globals_node = root_node->find_named_node("Globals");
	if (globals_node)
	{
		if ((prop = globals_node->property ("tempo")) != 0) {
			sscanf (prop->value().c_str(), "%lg", &_tempo);
		}
		if ((prop = globals_node->property ("eighth_per_cycle")) != 0) {
			sscanf (prop->value().c_str(), "%g", &_eighth_cycle);
		}
		if ((prop = globals_node->property ("common_dry")) != 0) {
			sscanf (prop->value().c_str(), "%g", &_curr_common_dry);
			_target_common_dry = _curr_common_dry;
		}
		if ((prop = globals_node->property ("common_wet")) != 0) {
			sscanf (prop->value().c_str(), "%g", &_curr_common_wet);
			_target_common_wet = _curr_common_wet;
		}
		if ((prop = globals_node->property ("input_gain")) != 0) {
			sscanf (prop->value().c_str(), "%g", &_curr_input_gain);
			_target_input_gain = _curr_input_gain;
		}
		if ((prop = globals_node->property ("auto_disable_latency")) != 0) {
			int temp = 0;
			sscanf (prop->value().c_str(), "%d", &temp);
			_auto_disable_latency = temp ? true: false;
		}
		if ((prop = globals_node->property ("sync_source")) != 0) {
			sscanf (prop->value().c_str(), "%d", (int *) (&_sync_source));
		}

	}
	
	
	// just remove everything for now
	while (_instances.size() > 0) {
		LoopManageEvent lmev (LoopManageEvent::RemoveLoop, _instances.back());
		// remove it now so indexes for new ones work out
		_instances.pop_back();
		// will be deleted later by us when the RT thread finishes with it
		push_loop_manage_to_rt (lmev);
	}

	XMLNode * loopers_node = root_node->find_named_node ("Loopers");
	if (!loopers_node) {
		return false;
	}
	
	looper_kids = loopers_node->children ("Looper");

	_loading = true;
	
	for (XMLNodeConstIterator niter = looper_kids.begin(); niter != looper_kids.end(); ++niter)
	{
		XMLNode *child;
		child = (*niter);

		Looper * instance = new Looper (_driver, *child);
		add_loop (instance);
	}

	_loading = false;

	_osc->send_all_config();
	
	return true;
}

bool
Engine::save_session (std::string fname, string * writestr)
{
	// make xmltree
	LocaleGuard lg ("POSIX");
	XMLTree sessiondoc;
	char buf[120];

	
	XMLNode * root_node = new XMLNode("SLSession");
	root_node->add_property("version", sooperlooper_version);
	sessiondoc.set_root (root_node);

	XMLNode * globals_node = root_node->add_child ("Globals");
	
	snprintf(buf, sizeof(buf), "%.10g", _tempo);
	globals_node->add_property ("tempo", buf);

	snprintf(buf, sizeof(buf), "%.10g", _eighth_cycle);
	globals_node->add_property ("eighth_per_cycle", buf);

	snprintf(buf, sizeof(buf), "%.10g", _curr_common_dry);
	globals_node->add_property ("common_dry", buf);
	
	snprintf(buf, sizeof(buf), "%.10g", _curr_common_wet);
	globals_node->add_property ("common_wet", buf);

	snprintf(buf, sizeof(buf), "%.10g", _curr_input_gain);
	globals_node->add_property ("input_gain", buf);
	
	snprintf(buf, sizeof(buf), "%d", (int)_sync_source);
	globals_node->add_property ("sync_source", buf);

	snprintf(buf, sizeof(buf), "%d", (int)_auto_disable_latency ? 1 : 0);
	globals_node->add_property ("auto_disable_latency", buf);
	
	
	XMLNode * loopers_node = root_node->add_child ("Loopers");

	for (Instances::iterator i = _instances.begin(); i != _instances.end(); ++i)
	{
		loopers_node->add_child_nocopy ((*i)->get_state());
	}


	if (writestr) {
		*writestr = sessiondoc.write_buffer();
	}
	
	// write doc to file
	if (!fname.empty()) {
		if (sessiondoc.write (fname))
		{	    
			fprintf (stderr, "Stored session as %s\n", fname.c_str());
			return true;
		}
		else {
			fprintf (stderr, "Failed to store session as %s\n", fname.c_str());
			return false;
		}
	}

	return true;
}
