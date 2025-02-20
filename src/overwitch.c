/*
 *   overwitch.c
 *   Copyright (C) 2021 David García Goñi <dagargo@gmail.com>
 *
 *   This file is part of Overwitch.
 *
 *   Overwitch is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Overwitch is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Overwitch. If not, see <http://www.gnu.org/licenses/>.
 */

#include "../config.h"
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <arpa/inet.h>
#include <math.h>
#include <jack/intclient.h>
#include <jack/thread.h>
#include <libgen.h>

#include "overbridge.h"

#define JACK_MAX_BUF_SIZE 128
//The lower the value, the lower the error at startup. If 1, there will be errors in the converters.
//Choosing a multiple of 2 might result in no error, which is undesirable.
#define MAX_READ_FRAMES 5

struct overbridge ob;
jack_client_t *client;
jack_port_t **output_ports;
jack_port_t **input_ports;
jack_nframes_t bufsize = 0;
double samplerate;
size_t o2j_buf_size;
size_t j2o_buf_size;
jack_default_audio_sample_t *j2o_buf_in;
jack_default_audio_sample_t *j2o_buf_out;
jack_default_audio_sample_t *j2o_aux;
jack_default_audio_sample_t *j2o_queue;
size_t j2o_queue_len;
jack_default_audio_sample_t *o2j_buf_in;
jack_default_audio_sample_t *o2j_buf_out;
SRC_STATE *j2o_state;
SRC_DATA j2o_data;
SRC_STATE *o2j_state;
SRC_DATA o2j_data;
size_t j2o_latency;
size_t o2j_latency;
int cycles_to_skip;
double jsr;
double obsr;
double j2o_ratio;
double o2j_ratio;
jack_nframes_t kj;
double _w0;
double _w1;
double _w2;
int kdel;
double _z1 = 0.0;
double _z2 = 0.0;
double _z3 = 0.0;
double o2j_ratio_max;
double o2j_ratio_min;
int read_frames;
int log_control_cycles;

void
overwitch_set_loop_filter (double bw)
{
  //Taken from https://github.com/jackaudio/tools/blob/master/zalsa/jackclient.cc.
  double w = 2.0 * M_PI * 20 * bw * bufsize / samplerate;
  _w0 = 1.0 - exp (-w);
  w = 2.0 * M_PI * bw * o2j_ratio / samplerate;
  _w1 = w * 1.6;
  _w2 = w * bufsize / 1.6;
}

int
overwitch_sample_rate_cb (jack_nframes_t nframes, void *arg)
{
  if (samplerate)
    {
      return 1;
    }

  samplerate = nframes;
  debug_print (0, "JACK sample rate: %.0f\n", samplerate);

  o2j_ratio = samplerate / OB_SAMPLE_RATE;
  j2o_ratio = 1.0 / o2j_ratio;
  o2j_ratio_max = 1.05 * o2j_ratio;
  o2j_ratio_min = 0.95 * o2j_ratio;

  return 0;
}

int
overwitch_buffer_size_cb (jack_nframes_t nframes, void *arg)
{
  if (bufsize)
    {
      return 1;
    }

  if (nframes > OB_FRAMES_PER_TRANSFER)
    {
      error_print
	("JACK buffer size is greater than device buffer size (%d > %d)\n",
	 nframes, OB_FRAMES_PER_TRANSFER);
      overbridge_set_status (&ob, OB_STATUS_STOP);
      return 1;
    }

  bufsize = nframes;
  debug_print (0, "JACK buffer size: %d\n", bufsize);

  kj = bufsize / -o2j_ratio;
  read_frames = bufsize * j2o_ratio;

  kdel = OB_FRAMES_PER_TRANSFER + 1.5 * bufsize;
  debug_print (2, "Target delay: %f ms (%d frames)\n",
	       kdel * 1000 / OB_SAMPLE_RATE, kdel);

  log_control_cycles = 2 * samplerate / bufsize;

  o2j_buf_size = bufsize * ob.o2j_frame_bytes;
  j2o_buf_size = bufsize * ob.j2o_frame_bytes;

  return 0;
}

static int
overwitch_thread_xrun_cb (void *arg)
{
  error_print ("JACK xrun\n");
  return 0;
}

static long
overwitch_j2o_reader (void *cb_data, float **data)
{
  long ret;

  *data = j2o_buf_in;

  if (j2o_queue_len == 0)
    {
      debug_print (2, "j2o: Can not read data from queue\n");
      return bufsize;
    }

  ret = j2o_queue_len;
  memcpy (j2o_buf_in, j2o_queue, j2o_queue_len * ob.j2o_frame_bytes);
  j2o_queue_len = 0;

  return ret;
}

static long
overwitch_o2j_reader (void *cb_data, float **data)
{
  size_t rso2j;
  size_t bytes;
  long frames;
  static int last_frames = 1;
  static int running = 0;

  *data = o2j_buf_in;

  rso2j = jack_ringbuffer_read_space (ob.o2j_rb);
  if (running)
    {
      if (o2j_latency < rso2j)
	{
	  o2j_latency = rso2j;
	}

      if (rso2j >= ob.o2j_frame_bytes)
	{
	  frames = rso2j / ob.o2j_frame_bytes;
	  frames = frames > MAX_READ_FRAMES ? MAX_READ_FRAMES : frames;
	  bytes = frames * ob.o2j_frame_bytes;
	  jack_ringbuffer_read (ob.o2j_rb, (void *) o2j_buf_in, bytes);
	}
      else
	{
	  debug_print (2,
		       "o2j: Can not read data from ring buffer. Replicating last sample...\n");
	  if (last_frames > 1)
	    {
	      memcpy (o2j_buf_in,
		      &o2j_buf_in[(last_frames - 1) * ob.device_desc.outputs],
		      ob.o2j_frame_bytes);
	    }
	  frames = MAX_READ_FRAMES;
	}
    }
  else
    {
      if (rso2j >= o2j_buf_size)
	{
	  jack_ringbuffer_read_advance (ob.o2j_rb, rso2j);
	  frames = bufsize;
	  running = 1;
	}
      else
	{
	  frames = MAX_READ_FRAMES;
	}
    }

  read_frames += frames;
  last_frames = frames;
  return frames;
}

static inline void
overwitch_o2j ()
{
  long gen_frames;

  read_frames = 0;
  gen_frames = src_callback_read (o2j_state, o2j_ratio, bufsize, o2j_buf_out);
  if (gen_frames != bufsize)
    {
      error_print
	("o2j: Unexpected frames with ratio %f (output %ld, expected %d)\n",
	 o2j_ratio, gen_frames, bufsize);
    }
}

static inline void
overwitch_j2o ()
{
  long gen_frames;
  int inc;
  int frames;
  size_t bytes;
  size_t wsj2o;
  overbridge_status_t status;
  static double j2o_acc = .0;

  pthread_spin_lock (&ob.lock);
  status = ob.status;
  pthread_spin_unlock (&ob.lock);

  memcpy (&j2o_queue[j2o_queue_len], j2o_aux, j2o_buf_size);
  j2o_queue_len += bufsize;

  j2o_acc += bufsize * (j2o_ratio - 1.0);
  inc = trunc (j2o_acc);
  j2o_acc -= inc;
  frames = bufsize + inc;

  gen_frames = src_callback_read (j2o_state, j2o_ratio, frames, j2o_buf_out);
  if (gen_frames != frames)
    {
      error_print
	("j2o: Unexpected frames with ratio %f (output %ld, expected %d)\n",
	 j2o_ratio, gen_frames, frames);
    }

  if (status < OB_STATUS_RUN)
    {
      return;
    }

  bytes = gen_frames * ob.j2o_frame_bytes;
  wsj2o = jack_ringbuffer_write_space (ob.j2o_rb);
  if (bytes <= wsj2o)
    {
      jack_ringbuffer_write (ob.j2o_rb, (void *) j2o_buf_out, bytes);
    }
  else
    {
      error_print ("j2o: Buffer overflow. Discarding data...\n");
    }
}

static inline void
overwitch_compute_ratios ()
{
  jack_nframes_t current_frames;
  jack_time_t current_usecs;
  jack_time_t next_usecs;
  float period_usecs;
  jack_nframes_t ko0;
  jack_nframes_t ko1;
  double to0;
  double to1;
  double tj;
  overbridge_status_t status;
  static int i = 0;
  static double sum_o2j_ratio = 0.0;
  static double sum_j2o_ratio = 0.0;
  static double last_o2j_ratio = 0.0;

  if (jack_get_cycle_times (client,
			    &current_frames,
			    &current_usecs, &next_usecs, &period_usecs))
    {
      error_print ("Error while getting JACK time\n");
    }

  pthread_spin_lock (&ob.lock);
  j2o_latency = ob.j2o_latency;
  ko0 = ob.i0.frames;
  to0 = ob.i0.time;
  ko1 = ob.i1.frames;
  to1 = ob.i1.time;
  status = ob.status;
  pthread_spin_unlock (&ob.lock);

  //The whole calculation of the delay and the loop filter is taken from https://github.com/jackaudio/tools/blob/master/zalsa/jackclient.cc.
  kj += read_frames;
  tj = current_usecs * 1.0e-6;

  jack_nframes_t n = ko1 - ko0;
  double dob = n * (tj - to0) / (to1 - to0);
  n = ko0 - kj;
  double err = n + dob - kdel;

  _z1 += _w0 * (_w1 * err - _z1);
  _z2 += _w0 * (_z1 - _z2);
  _z3 += _w2 * _z2;
  o2j_ratio = 1.0 - _z2 - _z3;
  if (o2j_ratio > o2j_ratio_max)
    {
      o2j_ratio = o2j_ratio_max;
    }
  if (o2j_ratio < o2j_ratio_min)
    {
      o2j_ratio = o2j_ratio_min;
    }
  j2o_ratio = 1.0 / o2j_ratio;

  i++;
  sum_o2j_ratio += o2j_ratio;
  sum_j2o_ratio += j2o_ratio;
  if (i == log_control_cycles)
    {
      debug_print (1,
		   "max. latencies (ms): %.1f, %.1f; avg. ratios: %f, %f\n",
		   o2j_latency * 1000.0 / (ob.o2j_frame_bytes *
					   OB_SAMPLE_RATE),
		   j2o_latency * 1000.0 / (ob.j2o_frame_bytes *
					   OB_SAMPLE_RATE),
		   sum_o2j_ratio / log_control_cycles,
		   sum_j2o_ratio / log_control_cycles);

      i = 0;
      sum_o2j_ratio = 0.0;
      sum_j2o_ratio = 0.0;

      if (status == OB_STATUS_STARTUP)
	{
	  debug_print (2, "Retunning loop filter...\n");
	  overwitch_set_loop_filter (0.05);

	  //Taken from https://github.com/jackaudio/tools/blob/master/zalsa/jackclient.cc.
	  int n = (int) (floor (err + 0.5));
	  kj += n;
	  err -= n;

	  pthread_spin_lock (&ob.lock);
	  ob.status = OB_STATUS_TUNE;
	  pthread_spin_unlock (&ob.lock);

	  last_o2j_ratio = o2j_ratio;

	  return;
	}
    }

  if (status == OB_STATUS_TUNE
      && abs (last_o2j_ratio - o2j_ratio) < 0.0000001)
    {
      pthread_spin_lock (&ob.lock);
      ob.status = OB_STATUS_RUN;
      pthread_spin_unlock (&ob.lock);
    }

  if (status < OB_STATUS_RUN)
    {
      last_o2j_ratio = o2j_ratio;
    }
}

static inline int
overwitch_process_cb (jack_nframes_t nframes, void *arg)
{
  jack_default_audio_sample_t *buffer[OB_MAX_TRACKS];
  float *f;

  overwitch_compute_ratios ();

  //o2j

  overwitch_o2j ();

  f = o2j_buf_out;
  for (int i = 0; i < ob.device_desc.outputs; i++)
    {
      buffer[i] = jack_port_get_buffer (output_ports[i], nframes);
    }
  for (int i = 0; i < nframes; i++)
    {
      for (int j = 0; j < ob.device_desc.outputs; j++)
	{
	  buffer[j][i] = *f;
	  f++;
	}
    }

  //j2o

  f = j2o_aux;
  for (int i = 0; i < ob.device_desc.inputs; i++)
    {
      buffer[i] = jack_port_get_buffer (input_ports[i], nframes);
    }
  for (int i = 0; i < nframes; i++)
    {
      for (int j = 0; j < ob.device_desc.inputs; j++)
	{
	  *f = buffer[j][i];
	  f++;
	}
    }

  overwitch_j2o ();

  return 0;
}

static void
overwitch_exit (int signo)
{
  debug_print (0, "Max. latencies (ms): %.1f, %.1f\n",
	       o2j_latency * 1000.0 / (ob.o2j_frame_bytes * OB_SAMPLE_RATE),
	       j2o_latency * 1000.0 / (ob.j2o_frame_bytes * OB_SAMPLE_RATE));

  overbridge_set_status (&ob, OB_STATUS_STOP);
}

static int
overwitch_run ()
{
  jack_options_t options = JackNullOption;
  jack_status_t status;
  int ob_status;
  char *client_name;
  struct sigaction action;
  size_t o2j_buf_max_size;
  size_t j2o_buf_max_size;
  int ret = 0;

  action.sa_handler = overwitch_exit;
  sigemptyset (&action.sa_mask);
  action.sa_flags = 0;
  sigaction (SIGHUP, &action, NULL);
  sigaction (SIGINT, &action, NULL);
  sigaction (SIGTERM, &action, NULL);

  ob_status = overbridge_init (&ob);
  if (ob_status)
    {
      error_print ("Device error: %s\n", overbrigde_get_err_str (ob_status));
      exit (EXIT_FAILURE);
    }

  client = jack_client_open (ob.device_desc.name, options, &status, NULL);
  if (client == NULL)
    {
      error_print ("jack_client_open() failed, status = 0x%2.0x\n", status);

      if (status & JackServerFailed)
	{
	  error_print ("Unable to connect to JACK server\n");
	}

      ret = EXIT_FAILURE;
      goto cleanup_overbridge;
    }

  if (status & JackServerStarted)
    {
      debug_print (0, "JACK server started\n");
    }

  if (status & JackNameNotUnique)
    {
      client_name = jack_get_client_name (client);
      debug_print (0, "Name client in use. Using %s...\n", client_name);
    }

  if (jack_set_process_callback (client, overwitch_process_cb, NULL))
    {
      ret = EXIT_FAILURE;
      goto cleanup_jack;
    }

  if (jack_set_xrun_callback (client, overwitch_thread_xrun_cb, NULL))
    {
      ret = EXIT_FAILURE;
      goto cleanup_jack;
    }

  if (jack_set_buffer_size_callback (client, overwitch_buffer_size_cb, NULL))
    {
      ret = EXIT_FAILURE;
      goto cleanup_jack;
    }

  if (jack_set_sample_rate_callback (client, overwitch_sample_rate_cb, NULL))
    {
      ret = EXIT_FAILURE;
      goto cleanup_jack;
    }

  output_ports = malloc (sizeof (jack_port_t *) * ob.device_desc.outputs);
  for (int i = 0; i < ob.device_desc.outputs; i++)
    {
      output_ports[i] =
	jack_port_register (client,
			    ob.device_desc.output_track_names[i],
			    JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

      if (output_ports[i] == NULL)
	{
	  error_print ("No more JACK ports available\n");
	  ret = EXIT_FAILURE;
	  goto cleanup_jack;
	}
    }

  input_ports = malloc (sizeof (jack_port_t *) * ob.device_desc.inputs);
  for (int i = 0; i < ob.device_desc.inputs; i++)
    {
      input_ports[i] =
	jack_port_register (client,
			    ob.device_desc.input_track_names[i],
			    JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);

      if (input_ports[i] == NULL)
	{
	  error_print ("No more JACK ports available\n");
	  ret = EXIT_FAILURE;
	  goto cleanup_jack;
	}
    }

  j2o_state =
    src_callback_new (overwitch_j2o_reader, SRC_SINC_FASTEST,
		      ob.device_desc.inputs, NULL, NULL);
  o2j_state =
    src_callback_new (overwitch_o2j_reader, SRC_SINC_FASTEST,
		      ob.device_desc.outputs, NULL, NULL);

  j2o_buf_max_size =
    JACK_MAX_BUF_SIZE * OB_BYTES_PER_FRAME * ob.device_desc.inputs;
  j2o_buf_in = malloc (j2o_buf_max_size);
  j2o_buf_out = malloc (j2o_buf_max_size * 4.5);	//Up to 192 kHz and a few samples more
  j2o_aux = malloc (j2o_buf_max_size);
  j2o_queue = malloc (j2o_buf_max_size * 4.5);	//Up to 192 kHz and a few samples more
  j2o_queue_len = 0;

  o2j_buf_max_size =
    JACK_MAX_BUF_SIZE * OB_BYTES_PER_FRAME * ob.device_desc.outputs;
  o2j_buf_in = malloc (o2j_buf_max_size);
  o2j_buf_out = malloc (o2j_buf_max_size);

  memset (j2o_buf_in, 0, j2o_buf_max_size);
  memset (o2j_buf_in, 0, o2j_buf_max_size);

  if (overbridge_run (&ob, client))
    {
      ret = EXIT_FAILURE;
      goto cleanup_jack;
    }

  overwitch_set_loop_filter (1.0);

  if (jack_activate (client))
    {
      error_print ("Cannot activate client\n");
      ret = EXIT_FAILURE;
      goto cleanup_jack;
    }

  overbridge_wait (&ob);

  debug_print (0, "Exiting...\n");
  jack_deactivate (client);

cleanup_jack:
  jack_client_close (client);
  src_delete (j2o_state);
  src_delete (o2j_state);
  free (output_ports);
  free (input_ports);
  free (j2o_buf_in);
  free (j2o_buf_out);
  free (j2o_aux);
  free (j2o_queue);
  free (o2j_buf_in);
  free (o2j_buf_out);
cleanup_overbridge:
  overbridge_destroy (&ob);

  return ret;
}

void
overwitch_help (char *executable_path)
{
  char *exec_name;

  fprintf (stderr, "%s\n", PACKAGE_STRING);
  exec_name = basename (executable_path);
  fprintf (stderr, "Usage: %s [-r ratio] [-v]\n", exec_name);
}

int
main (int argc, char *argv[])
{
  int c;
  int vflg = 0, errflg = 0;

  while ((c = getopt (argc, argv, "hv")) != -1)
    {
      switch (c)
	{
	case 'h':
	  overwitch_help (argv[0]);
	  exit (EXIT_SUCCESS);
	case 'v':
	  vflg++;
	  break;
	case '?':
	  errflg++;
	}
    }

  if (vflg)
    {
      debug_level = vflg;
    }

  if (errflg > 0)
    {
      overwitch_help (argv[0]);
      exit (EXIT_FAILURE);
    }

  return overwitch_run ();
}
