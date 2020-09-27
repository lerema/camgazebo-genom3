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


/* --- Calib helper  ------------------------------------------------------ */
void compute_calib(or_sensor_intrinsics* intr, float hfov, camgazebo_ids_img_size size)
{
    float f = size.w/2/tan(hfov/2);
    intr->calib = {
        f, f, (float)size.w/2, (float)size.h/2, 0
    };
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
    ids->info.started = false;

    // These are the defaults values for the gazebo camera
    ids->hfov = 1.047;
    ids->size = {320, 240};

    ids->data = new or_camera_data(ids->size.w, ids->size.h);
    ids->pipe = new or_camera_pipe();

    if (genom_sequence_reserve(&(frame->data(self)->pixels), ids->data->l) == -1) {
        camgazebo_e_mem_detail d;
        snprintf(d.what, sizeof(d.what), "unable to allocate frame memory");
        warnx("%s", d.what);
        return camgazebo_e_mem(&d,self);
    }
    frame->data(self)->pixels._length = ids->data->l;
    frame->data(self)->height = ids->size.h;
    frame->data(self)->width = ids->size.w;
    frame->data(self)->bpp = 3;

    // Publish initial calibration
    compute_calib(intrinsics->data(self), ids->hfov, ids->size);

    intrinsics->write(self);
    extrinsics->write(self);

    return camgazebo_wait;
}


/** Codel camgz_wait of task main.
 *
 * Triggered by camgazebo_wait.
 * Yields to camgazebo_pause_wait, camgazebo_pub.
 */
genom_event
camgz_wait(bool started, const or_camera_data *data,
           const genom_context self)
{
    if (!started || !(data->new_frame))
        return camgazebo_pause_wait;
    return camgazebo_pub;
}


/** Codel camgz_pub of task main.
 *
 * Triggered by camgazebo_pub.
 * Yields to camgazebo_wait.
 */
genom_event
camgz_pub(or_camera_data **data, const camgazebo_frame *frame,
          const genom_context self)
{
    or_sensor_frame* fdata = frame->data(self);

    std::lock_guard<std::mutex> guard((*data)->lock);

    fdata->pixels._buffer = (uint8_t*)(*data)->data;
    fdata->ts.sec = (*data)->tv.tv_sec;
    fdata->ts.nsec = (*data)->tv.tv_usec * 1000;

    frame->write(self);

    (*data)->new_frame = false;

    return camgazebo_wait;
}


/* --- Activity connect ------------------------------------------------- */

/** Codel camgz_connect of activity connect.
 *
 * Triggered by camgazebo_start.
 * Yields to camgazebo_ether.
 */
genom_event
camgz_connect(const char topic[256], or_camera_data **data,
              or_camera_pipe **pipe,
              const camgazebo_intrinsics *intrinsics, bool *started,
              const genom_context self)
{
    if (*started)
        warnx("already connected to gazebo, disconnect() first");
    else
    {
        gazebo::client::setup();
        (*pipe)->node = gazebo::transport::NodePtr(new gazebo::transport::Node());
        (*pipe)->node->Init();

        (*pipe)->sub = (*pipe)->node->Subscribe(topic, &or_camera_data::cb, *data);

        warnx("connected to %s", topic);
        *started = true;
    }

    return camgazebo_ether;
}


/* --- Activity disconnect ---------------------------------------------- */

/** Codel camgz_disconnect of activity disconnect.
 *
 * Triggered by camgazebo_start.
 * Yields to camgazebo_ether.
 */
genom_event
camgz_disconnect(or_camera_data **data, bool *started,
                 const genom_context self)
{
    std::lock_guard<std::mutex> guard((*data)->lock);

    gazebo::client::shutdown();
    *started = false;

    warnx("disconnected from gazebo");

    return camgazebo_ether;
}


/* --- Activity set_extrinsics ------------------------------------------ */

/** Codel camgz_set_extrinsics of activity set_extrinsics.
 *
 * Triggered by camgazebo_start.
 * Yields to camgazebo_ether.
 */
genom_event
camgz_set_extrinsics(const sequence6_float *ext_values,
                     const camgazebo_extrinsics *extrinsics,
                     const genom_context self)
{
    *extrinsics->data(self) = {
        ext_values->_buffer[0],
        ext_values->_buffer[1],
        ext_values->_buffer[2],
        ext_values->_buffer[3],
        ext_values->_buffer[4],
        ext_values->_buffer[5]
    };

    extrinsics->write(self);

    warnx("new extrinsic calibration");

    return camgazebo_ether;
}


/* --- Activity set_hfov ------------------------------------------------ */

/** Codel camgz_set_hfov of activity set_hfov.
 *
 * Triggered by camgazebo_start.
 * Yields to camgazebo_ether.
 */
genom_event
camgz_set_hfov(float hfov_val, float *hfov,
               const camgazebo_ids_img_size *size,
               const camgazebo_intrinsics *intrinsics,
               const genom_context self)
{
    *hfov = hfov_val;

    compute_calib(intrinsics->data(self), *hfov, *size);


    intrinsics->write(self);

    warnx("new intrinsic calibration");

    return camgazebo_ether;
}


/* --- Activity set_format ---------------------------------------------- */

/** Codel camgz_set_fmt of activity set_format.
 *
 * Triggered by camgazebo_start.
 * Yields to camgazebo_ether.
 */
genom_event
camgz_set_fmt(uint16_t w_val, uint16_t h_val, or_camera_data **data,
              float hfov, camgazebo_ids_img_size *size,
              const camgazebo_frame *frame,
              const camgazebo_intrinsics *intrinsics,
              const genom_context self)
{
    *size = {w_val, h_val};

    (*data)->set_size(w_val, h_val);

    if (genom_sequence_reserve(&(frame->data(self)->pixels), (*data)->l) == -1) {
        camgazebo_e_mem_detail d;
        snprintf(d.what, sizeof(d.what), "unable to allocate frame memory");
        warnx("%s", d.what);
        return camgazebo_e_mem(&d,self);
    }
    frame->data(self)->pixels._length = (*data)->l;
    frame->data(self)->height = h_val;
    frame->data(self)->width = w_val;


    compute_calib(intrinsics->data(self), hfov, *size);
    intrinsics->write(self);

    warnx("new intrinsic calibration");

    return camgazebo_ether;
}


/* --- Activity set_disto ----------------------------------------------- */

/** Codel camgz_set_disto of activity set_disto.
 *
 * Triggered by camgazebo_start.
 * Yields to camgazebo_ether.
 */
genom_event
camgz_set_disto(const sequence5_float *dist_values,
                const camgazebo_intrinsics *intrinsics,
                const genom_context self)
{
    intrinsics->data(self)->disto = {
        dist_values->_buffer[0],
        dist_values->_buffer[1],
        dist_values->_buffer[2],
        dist_values->_buffer[3],
        dist_values->_buffer[4]
    };

    intrinsics->write(self);

    warnx("new intrinsic calibration");

    return camgazebo_ether;
}
