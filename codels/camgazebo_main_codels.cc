/*
 * Copyright (c) 2020 LAAS/CNRS
 * All rights reserved.
 *
 * Redistribution  and  use  in  source  and binary  forms,  with  or  without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of  source  code must retain the  above copyright
 *      notice and this list of conditions.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice and  this list of  conditions in the  documentation and/or
 *      other materials provided with the distribution.
 *
 * THE SOFTWARE  IS PROVIDED "AS IS"  AND THE AUTHOR  DISCLAIMS ALL WARRANTIES
 * WITH  REGARD   TO  THIS  SOFTWARE  INCLUDING  ALL   IMPLIED  WARRANTIES  OF
 * MERCHANTABILITY AND  FITNESS.  IN NO EVENT  SHALL THE AUTHOR  BE LIABLE FOR
 * ANY  SPECIAL, DIRECT,  INDIRECT, OR  CONSEQUENTIAL DAMAGES  OR  ANY DAMAGES
 * WHATSOEVER  RESULTING FROM  LOSS OF  USE, DATA  OR PROFITS,  WHETHER  IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR  OTHER TORTIOUS ACTION, ARISING OUT OF OR
 * IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *                                                  Martin Jacquet - June 2020
 */
#include "accamgazebo.h"

#include "camgazebo_c_types.h"

#include "codels.hpp"
#include <iostream>
#include <mutex>

using std::cout;
using std::endl;

/* --- Global variables ------------------------------------------------- */
or_camera_data* cgz_data;
std::mutex mut;

/* --- Callback --------------------------------------------------------- */
void camgz_cb(ConstImageStampedPtr& _msg)
{
    //data.length == width * step ??
    if (_msg->image().data().length() != cgz_data->l)
        warnx("Skipping frame, incorrect size");
    else
    {
        std::lock_guard<std::mutex> guard(mut);
        memcpy(cgz_data->data, _msg->image().data().c_str(), _msg->image().data().length());
    }
}

/* --- Calib ------------------------------------------------------------ */
void compute_calib(or_sensor_intrinsics* intr, double hfov, uint16_t w, uint16_t h)
{
    double vfov = hfov*h/w;

    intr->calib._buffer[0] = w/2/tan(hfov/2);
    intr->calib._buffer[1] = h/2/tan(vfov/2);
    intr->calib._buffer[2] = 0;
    intr->calib._buffer[3] = w/2;
    intr->calib._buffer[4] = h/2;
}

/* --- Task main -------------------------------------------------------- */


/** Codel camgz_start of task main.
 *
 * Triggered by camgazebo_start.
 * Yields to camgazebo_wait.
 */
genom_event
camgz_start(camgazebo_ids *ids, const camgazebo_frame *frame,
            const camgazebo_extrinsics *extrinsics,
            const camgazebo_intrinsics *intrinsics,
            const genom_context self)
{
    cgz_data = new or_camera_data();

    ids->pipe = new or_camera_pipe();
    ids->info.started = false;
    ids->hfov = 1.047;
    ids->w = 1920;
    ids->h = 1080;

    // Init values for extrinsics
    *extrinsics->data(self) = {0,0,0, 0,0,0};
    extrinsics->write(self);

    // Init values for intrinsics
    or_sensor_intrinsics* intr = intrinsics->data(self);
    compute_calib(intr, ids->hfov, ids->w, ids->h);
    intr->disto._buffer[0] = 0;
    intr->disto._buffer[1] = 0;
    intr->disto._buffer[2] = 0;
    intr->disto._buffer[3] = 0;
    intr->disto._buffer[4] = 0;

    *intrinsics->data(self) = *intr;
    intrinsics->write(self);

    return camgazebo_wait;
}


/** Codel camgz_wait of task main.
 *
 * Triggered by camgazebo_wait.
 * Yields to camgazebo_pause_wait, camgazebo_pub.
 */
genom_event
camgz_wait(bool started, const genom_context self)
{
    if (started)
        return camgazebo_pub;
    else
        return camgazebo_pause_wait;
}


/** Codel camgz_pub of task main.
 *
 * Triggered by camgazebo_pub.
 * Yields to camgazebo_pause_wait.
 */
genom_event
camgz_pub(uint16_t h, uint16_t w, const camgazebo_frame *frame,
          const genom_context self)
{
    or_sensor_frame* fdata = frame->data(self);

    if (cgz_data->l > fdata->pixels._maximum)
        if (genom_sequence_reserve(&(fdata->pixels), cgz_data->l) == -1) {
            camgazebo_e_mem_detail d;
            snprintf(d.what, sizeof(d.what), "unable to allocate frame memory");
            printf("camgazebo: %s\n", d.what);
            return camgazebo_e_mem(&d,self);
        }
    fdata->height = h;
    fdata->width = w;
    fdata->bpp = 3;
    fdata->pixels._length = cgz_data->l;

    std::lock_guard<std::mutex> guard(mut);
    memcpy(fdata->pixels._buffer, cgz_data->data, cgz_data->l);

    *(frame->data(self)) = *fdata;
    frame->write(self);

    return camgazebo_pause_wait;
}


/* --- Activity connect ------------------------------------------------- */

/** Codel camgz_connect of activity connect.
 *
 * Triggered by camgazebo_start.
 * Yields to camgazebo_ether.
 */
genom_event
camgz_connect(const char world[64], const char model[64],
              const char link[64], const char sensor[64],
              or_camera_pipe **pipe,
              const camgazebo_intrinsics *intrinsics, bool *started,
              const genom_context self)
{
    std::lock_guard<std::mutex> guard(mut);

    gazebo::client::setup();
    (*pipe)->node = gazebo::transport::NodePtr(new gazebo::transport::Node());
    (*pipe)->node->Init();

    char* topic = new char[256];
    snprintf(topic, 256, "/gazebo/%s/%s/%s/%s/image", world, model, link, sensor);
    (*pipe)->sub = (*pipe)->node->Subscribe(topic, camgz_cb);

    warnx("connected to %s", topic);
    *started = true;

    return camgazebo_ether;
}


/* --- Activity disconnect ---------------------------------------------- */

/** Codel camgz_disconnect of activity disconnect.
 *
 * Triggered by camgazebo_start.
 * Yields to camgazebo_ether.
 */
genom_event
camgz_disconnect(bool *started, const genom_context self)
{
    std::lock_guard<std::mutex> guard(mut);
    gazebo::client::shutdown();
    *started = false;

    return camgazebo_ether;
}


/* --- Activity set_extrinsics ------------------------------------------ */

/** Codel camgz_set_extrinsics of activity set_extrinsics.
 *
 * Triggered by camgazebo_start.
 * Yields to camgazebo_ether.
 */
genom_event
camgz_set_extrinsics(const sequence6_double *ext_values,
                     const camgazebo_extrinsics *extrinsics,
                     const genom_context self)
{
    or_sensor_extrinsics ext;
    ext = {ext_values->_buffer[0],
           ext_values->_buffer[1],
           ext_values->_buffer[2],
           ext_values->_buffer[3],
           ext_values->_buffer[4],
           ext_values->_buffer[5]};

    *extrinsics->data(self) = ext;
    extrinsics->write(self);

    return camgazebo_ether;
}


/* --- Activity set_hfov ------------------------------------------------ */

/** Codel camgz_set_hfov of activity set_hfov.
 *
 * Triggered by camgazebo_start.
 * Yields to camgazebo_ether.
 */
genom_event
camgz_set_hfov(float hfov_val, float *hfov, uint16_t h, uint16_t w,
               const camgazebo_intrinsics *intrinsics,
               const genom_context self)
{
    *hfov = hfov_val;

    or_sensor_intrinsics* intr = intrinsics->data(self);
    compute_calib(intr, *hfov, w, h);
    *intrinsics->data(self) = *intr;
    intrinsics->write(self);

    return camgazebo_ether;
}


/* --- Activity set_format ---------------------------------------------- */

/** Codel camgz_set_fmt of activity set_format.
 *
 * Triggered by camgazebo_start.
 * Yields to camgazebo_ether.
 */
genom_event
camgz_set_fmt(uint16_t w_val, uint16_t *w, uint16_t h_val, uint16_t *h,
              float hfov, const camgazebo_intrinsics *intrinsics,
              const genom_context self)
{
    *w = w_val;
    *h = h_val;

    cgz_data->l = *h * *w * 3;

    or_sensor_intrinsics* intr = intrinsics->data(self);
    compute_calib(intr, hfov, *w, *h);
    *intrinsics->data(self) = *intr;
    intrinsics->write(self);

    return camgazebo_ether;
}


/* --- Activity set_disto ----------------------------------------------- */

/** Codel camgz_set_disto of activity set_disto.
 *
 * Triggered by camgazebo_start.
 * Yields to camgazebo_ether.
 */
genom_event
camgz_set_disto(const sequence5_double *dist_values,
                const camgazebo_intrinsics *intrinsics,
                const genom_context self)
{
    or_sensor_intrinsics* intr = intrinsics->data(self);

    intr->disto._buffer[0] = dist_values->_buffer[0];
    intr->disto._buffer[1] = dist_values->_buffer[1];
    intr->disto._buffer[2] = dist_values->_buffer[2];
    intr->disto._buffer[3] = dist_values->_buffer[3];
    intr->disto._buffer[4] = dist_values->_buffer[4];

    *intrinsics->data(self) = *intr;
    intrinsics->write(self);

    warnx("camgazebo: new distorsion parameters");

    return camgazebo_ether;
}
