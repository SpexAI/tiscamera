/*
 * Copyright 2020 The Imaging Source Europe GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "gsttcamsrc.h"

#include "../../../libs/tcamprop/src/tcam-property-1.0.h"
#include "../../base_types.h"
#include "../../logging.h"
#include "../../public_utils.h"
#include "../tcamgstbase/tcamgstjson.h"
#include "tcambind.h"
#include "tcamsrc_tcamprop_impl.h"

#include <gst-helper/gst_gvalue_helper.h>
#include <gst-helper/gst_ptr.h>
#include <unistd.h>

using namespace tcam;

namespace tcamsrc
{
struct tcamsrc_state
{
    GstElement* active_source = nullptr;
    GstDeviceMonitor* p_monitor = nullptr;
    GstPad* pad = nullptr;

    gst_helper::gst_ptr<GstDevice> device_to_open;

    std::string device_serial;
    tcam::TCAM_DEVICE_TYPE device_type = TCAM_DEVICE_TYPE_UNKNOWN;

    gint cam_buffers = 10;
    gboolean drop_incomplete_frames = true;
    gboolean do_timestamp = false;
    gint num_buffers = -1;
};
} // namespace tcamsrc


GST_DEBUG_CATEGORY_STATIC(tcam_src_debug);
#define GST_CAT_DEFAULT tcam_src_debug


static gboolean open_source_element(GstTcamSrc* self);


#define gst_tcam_src_parent_class parent_class

G_DEFINE_TYPE_WITH_CODE(GstTcamSrc,
                        gst_tcam_src,
                        GST_TYPE_BIN,
                        G_IMPLEMENT_INTERFACE(TCAM_TYPE_PROPERTY_PROVIDER,
                                              tcam::gst::src::gst_tcam_src_prop_init))

enum
{
    SIGNAL_DEVICE_OPEN,
    SIGNAL_DEVICE_CLOSE,
    SIGNAL_LAST,
};

static guint gst_tcamsrc_signals[SIGNAL_LAST] = {
    0,
};


enum
{
    PROP_0,
    PROP_SERIAL,
    PROP_DEVICE_TYPE,
    PROP_CAM_BUFFERS,
    PROP_NUM_BUFFERS,
    PROP_DO_TIMESTAMP,
    PROP_DROP_INCOMPLETE_FRAMES,
    PROP_STATE,
    PROP_TCAMDEVICE,
};

static tcamsrc::tcamsrc_state& get_element_state(GstTcamSrc* self)
{
    return *self->state;
}

GstElement* tcamsrc::get_active_source(GstTcamSrc* self)
{
    return self->state->active_source;
}

static GstStaticPadTemplate tcam_src_template =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("ANY"));

static void close_active_source_element(GstTcamSrc* self)
{
    if (self->state->active_source)
    {
        gst_element_set_state(self->state->active_source, GST_STATE_NULL);
        // TODO causes critical error  g_object_ref: assertion 'old_val > 0' failed
        // gst_bin_remove(GST_BIN(self), self->active_source);

        //gst_object_unref(self->active_source);
        self->state->active_source = nullptr;
    }
}

static void apply_element_property(GstTcamSrc* self,
                                   guint prop_id,
                                   const GValue* value,
                                   GParamSpec* pspec)
{
    auto& state = get_element_state(self);

    auto is_state_null = [&]
    {
        GstState cur_gst_state = GST_STATE_NULL;
        auto res = gst_element_get_state(GST_ELEMENT(self), &cur_gst_state, NULL, 0);
        if (res == GST_STATE_CHANGE_FAILURE || res == GST_STATE_CHANGE_ASYNC)
        {
            return false;
        }
        return cur_gst_state == GST_STATE_NULL;
    };

    switch (prop_id)
    {
        case PROP_SERIAL:
        {
            if (!is_state_null())
            {
                GST_ERROR_OBJECT(self,
                                 "The gobject property 'serial' can only be set in GST_STATE_NULL");
                return;
            }

            if (g_value_get_string(value) == NULL)
            {
                state.device_serial.clear();
            }
            else
            {
                std::string string_value = g_value_get_string(value);

                auto [s, t] = tcambind::separate_serial_and_type(string_value);
                if (!t.empty())
                {
                    auto type = tcam::tcam_device_from_string(t);
                    GST_INFO("Serial-Type input detected. Using serial: '%s' type: '%s' (from %s)",
                             s.c_str(),
                             tcam::tcam_device_type_to_string(type).c_str(),
                             string_value.c_str());

                    state.device_serial = s;
                    state.device_type = type;

                    GST_DEBUG("Type interpreted as '%s'",
                              tcam::tcam_device_type_to_string(state.device_type).c_str());
                }
                else
                {
                    state.device_serial = string_value;
                    //state.device_type = TCAM_DEVICE_TYPE_UNKNOWN;
                }
            }

            GST_INFO("Set camera serial to %s", state.device_serial.c_str());
            break;
        }
        case PROP_DEVICE_TYPE:
        {
            if (!is_state_null())
            {
                GST_ERROR_OBJECT(self,
                                 "The gobject property 'type' can only be set in GST_STATE_NULL");
                return;
            }

            const char* type = g_value_get_string(value);
            if (!type)
            {
                return;
            }

            // this check is simply for messaging the user
            // about invalid values
            auto vec = tcam::get_device_type_list_strings();
            if (std::find(vec.begin(), vec.end(), std::string(type)) == vec.end())
            {
                GST_ERROR("Unknown device type '%s'", type);
                state.device_type = TCAM_DEVICE_TYPE_UNKNOWN;
            }
            else
            {
                GST_DEBUG("Setting device type to %s", type);
                state.device_type = tcam::tcam_device_from_string(type);
            }
            break;
        }
        case PROP_CAM_BUFFERS:
        {
            if (state.active_source
                && g_object_class_find_property(G_OBJECT_GET_CLASS(state.active_source),
                                                "camera-buffers"))
            {
                g_object_set_property(G_OBJECT(state.active_source), "camera-buffers", value);
            }
            else
            {
                if (state.active_source)
                {
                    GST_INFO("Used source element does not support \"camera-buffers\".");
                }
                else
                {
                    state.cam_buffers = g_value_get_int(value);
                }
            }

            break;
        }
        case PROP_NUM_BUFFERS:
        {
            if (state.active_source
                && g_object_class_find_property(G_OBJECT_GET_CLASS(state.active_source),
                                                "num-buffers"))
            {
                g_object_set_property(G_OBJECT(state.active_source), "num-buffers", value);
            }
            else
            {
                if (state.active_source)
                {
                    GST_INFO("Used source element does not support \"num-buffers\".");
                }
                else
                {
                    state.num_buffers = g_value_get_int(value);
                }
            }

            break;
        }
        case PROP_DO_TIMESTAMP:
        {
            if (state.active_source
                && g_object_class_find_property(G_OBJECT_GET_CLASS(state.active_source),
                                                "do-timestamp"))
            {
                g_object_set_property(G_OBJECT(state.active_source), "do-timestamp", value);
            }
            else
            {
                state.do_timestamp = g_value_get_boolean(value);
            }
            break;
        }
        case PROP_DROP_INCOMPLETE_FRAMES:
        {
            if (state.active_source
                && g_object_class_find_property(G_OBJECT_GET_CLASS(state.active_source),
                                                "drop-incomplete-buffer"))
            {
                g_object_set_property(
                    G_OBJECT(state.active_source), "drop-incomplete-buffer", value);
            }
            else
            {
                if (state.active_source)
                {
                    GST_INFO("Used source element does not support \"drop-incomplete-buffer\"");
                }
                else
                {
                    state.drop_incomplete_frames = g_value_get_boolean(value);
                }
            }
            break;
        }
        case PROP_STATE:
        {
            if (state.active_source)
            {
                // if (state.active_source == state.main_src)
                // {
                //     g_object_set_property(G_OBJECT(state.active_source), "state", value);
                // }
                // else
                // {
                //     bool state = tcam::gst::load_device_settings(
                //         TCAM_PROP(self), state.device_serial, g_value_get_string(value));
                //     if (!state)
                //     {
                //         GST_WARNING("Device may be in an undefined state.");
                //     }
                // }
            }
            else
            {
                GST_WARNING("No active source.");
            }
            break;
        }
        case PROP_TCAMDEVICE:
        {
            if (!is_state_null())
            {
                GST_ERROR_OBJECT(
                    self, "The gobject property 'tcam-device' can only be set in GST_STATE_NULL");
                return;
            }

            auto ptr = GST_DEVICE(g_value_get_object(value));
            if (ptr == nullptr)
            {
                state.device_to_open.reset();
            }
            else
            {
                state.device_to_open = gst_helper::make_addref_ptr(ptr);
            }
            break;
        }
        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID(G_OBJECT(self), prop_id, pspec);
            break;
        }
    }
}


static void emit_device_open(GstElement* /*object*/, void* user_data)
{
    // emit our own instance of 'device-open'. user-data is the tcamsrc instance
    g_signal_emit(G_OBJECT(user_data), gst_tcamsrc_signals[SIGNAL_DEVICE_OPEN], 0);
}


static void emit_device_close(GstElement* /*object*/, void* user_data)
{
    // emit our own instance of 'device-open'. user-data is the tcamsrc instance
    g_signal_emit(G_OBJECT(user_data), gst_tcamsrc_signals[SIGNAL_DEVICE_CLOSE], 0);
}


namespace
{
struct device_id
{
    std::string serial;
    tcam::TCAM_DEVICE_TYPE type;
};

static device_id get_device_id(GstDevice& device)
{
    GstStructure* struc = gst_device_get_properties(&device);
    if (struc == nullptr)
    {
        return {};
    }

    auto serial = gst_helper::get_string_entry(*struc, "serial");
    auto type = gst_helper::get_string_entry(*struc, "tcam-device-type");

    gst_structure_free(struc);

    return { serial, tcam::tcam_device_from_string(type) };
}

} // namespace

static gboolean open_source_element(GstTcamSrc* self)
{
    auto& state = get_element_state(self);

    assert(state.active_source == nullptr);
    close_active_source_element(self);

    gst_helper::gst_ptr<GstDevice> selected_gst_device;
    if (state.device_to_open == nullptr)
    {
        /*
          How source selection works

          if serial exists -> use first matching source
          if serial && type exist -> use matching source
          if no serial && type -> matching source
          if no source && no type -> first source with device available

          mainsrc has prevalence over other sources for unspecified devices
         */
        GList* devices = gst_device_monitor_get_devices(state.p_monitor);
        if (!devices)
        {
            GST_ERROR("Failed to open tcamsrc device, but no devices in GstDeviceMonitor list");
            return FALSE;
        }

        auto find_device_for_params =
            [](GList* device_list,
               std::string serial_to_open,
               TCAM_DEVICE_TYPE device_type_to_open) -> gst_helper::gst_ptr<GstDevice>
        {
            for (GList* iter = device_list; iter != nullptr; iter = g_list_next(iter))
            {
                GstDevice* dev = static_cast<GstDevice*>(iter->data);
                if (dev == nullptr)
                {
                    continue;
                }

                auto device_id = get_device_id(*dev);

                if (!serial_to_open.empty() && serial_to_open != device_id.serial)
                {
                    continue;
                }
                if (device_type_to_open != TCAM_DEVICE_TYPE_UNKNOWN
                    && device_type_to_open != device_id.type)
                {
                    continue;
                }
                return gst_helper::make_addref_ptr(dev);
            }
            return nullptr;
        };

        selected_gst_device =
            find_device_for_params(devices, state.device_serial, state.device_type);

        g_list_free_full(devices, gst_object_unref);
    }
    else
    {
        selected_gst_device = state.device_to_open;
    }

    if (selected_gst_device == nullptr)
    {
        GST_ERROR(
            "Failed to find a device for the given serial='%s' and type='%s'. Stream not possible.",
            state.device_serial.c_str(),
            tcam::tcam_device_type_to_string(state.device_type).c_str());
        return FALSE;
    }

    state.active_source = gst_device_create_element(selected_gst_device.get(), nullptr);
    if (!state.active_source)
    {
        GST_ERROR("Unable to open a source element. Stream not possible.");
        return FALSE;
    }

    auto [serial, type] = get_device_id(*selected_gst_device);

    state.device_serial = serial;
    state.device_type = type;

    g_signal_connect(
        G_OBJECT(state.active_source), "device-open", G_CALLBACK(emit_device_open), self);

    g_signal_connect(
        G_OBJECT(state.active_source), "device-close", G_CALLBACK(emit_device_close), self);

    gst_bin_add(GST_BIN(self), state.active_source);
    // bin takes ownership over source element
    // we want to hold all source elements outside of
    // the bin for indexing purposes
    g_object_ref(state.active_source);

    gst_ghost_pad_set_target(GST_GHOST_PAD(state.pad), NULL);
    auto target_pad = gst_element_get_static_pad(state.active_source, "src");
    if (!gst_ghost_pad_set_target(GST_GHOST_PAD(state.pad), target_pad))
    {
        GST_ERROR("Could not set target for ghostpad.");
    }
    gst_object_unref(target_pad);

    gst_element_set_state(state.active_source, GST_STATE_READY);

    GValue val = G_VALUE_INIT;

    g_value_init(&val, G_TYPE_INT);
    g_value_set_int(&val, state.cam_buffers);
    // manually set all properties to ensure they are correctly applied
    apply_element_property(self, PROP_CAM_BUFFERS, &val, nullptr);

    // g_value_reset(&val);
    // g_value_init(&val, G_TYPE_INT);
    g_value_set_int(&val, state.num_buffers);

    apply_element_property(self, PROP_NUM_BUFFERS, &val, nullptr);

    // g_value_reset(&val);

    GValue val_bool = G_VALUE_INIT;

    g_value_init(&val_bool, G_TYPE_BOOLEAN);
    g_value_set_boolean(&val_bool, state.drop_incomplete_frames);

    apply_element_property(self, PROP_DROP_INCOMPLETE_FRAMES, &val_bool, nullptr);

    // g_value_reset(&val);
    // g_value_init(&val, G_TYPE_BOOLEAN);
    g_value_set_boolean(&val_bool, state.do_timestamp);

    apply_element_property(self, PROP_DO_TIMESTAMP, &val_bool, nullptr);

    GST_INFO("Opened device with serial: '%s' type: '%s'",
             state.device_serial.c_str(),
             tcam::tcam_device_type_to_string(state.device_type).c_str());

    return TRUE;
}


static GstStateChangeReturn gst_tcam_src_change_state(GstElement* element, GstStateChange change)
{
    GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

    GstTcamSrc* self = GST_TCAM_SRC(element);

    switch (change)
    {
        case GST_STATE_CHANGE_NULL_TO_READY:
        {
            if (!open_source_element(self))
            {
                return GST_STATE_CHANGE_FAILURE;
            }
            break;
        }
        default:
        {
            break;
        }
    }

    gst_element_set_locked_state(element, TRUE);
    ret = GST_ELEMENT_CLASS(gst_tcam_src_parent_class)->change_state(element, change);
    gst_element_set_locked_state(element, FALSE);

    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        return ret;
    }

    switch (change)
    {
        case GST_STATE_CHANGE_READY_TO_NULL:
        {
            close_active_source_element(self);
            break;
        }
        default:
            break;
    }
    return ret;
}

static void gst_tcam_src_set_property(GObject* object,
                                      guint prop_id,
                                      const GValue* value,
                                      GParamSpec* pspec)
{
    GstTcamSrc* self = GST_TCAM_SRC(object);

    apply_element_property(self, prop_id, value, pspec);
}


static void gst_tcam_src_get_property(GObject* object,
                                      guint prop_id,
                                      GValue* value,
                                      GParamSpec* pspec)
{
    GstTcamSrc* self = GST_TCAM_SRC(object);

    auto& state = get_element_state(self);

    switch (prop_id)
    {
        case PROP_SERIAL:
        {
            g_value_set_string(value, state.device_serial.c_str());
            break;
        }
        case PROP_DEVICE_TYPE:
        {
            if (state.active_source
                && g_object_class_find_property(G_OBJECT_GET_CLASS(state.active_source), "type"))
            {
                g_object_get_property(G_OBJECT(state.active_source), "type", value);
            }
            else
            {
                g_value_set_string(value,
                                   tcam::tcam_device_type_to_string(state.device_type).c_str());
            }
            break;
        }
        case PROP_CAM_BUFFERS:
        {
            if (state.active_source)
            {
                if (state.active_source
                    && g_object_class_find_property(G_OBJECT_GET_CLASS(state.active_source),
                                                    "camera-buffers"))
                {
                    g_object_get_property(G_OBJECT(state.active_source), "camera-buffers", value);
                }
                else
                {
                    GST_WARNING("Source element does not support camera-buffers.");
                }
            }
            else
            {
                GST_WARNING("No active source.");
            }
            break;
        }
        case PROP_NUM_BUFFERS:
        {
            if (state.active_source)
            {
                if (state.active_source
                    && g_object_class_find_property(G_OBJECT_GET_CLASS(state.active_source),
                                                    "num-buffers"))
                {
                    g_object_get_property(G_OBJECT(state.active_source), "num-buffers", value);
                }
                else
                {
                    GST_WARNING("Source element does not support num-buffers.");
                }
            }
            else
            {
                GST_WARNING("No active source.");
            }
            break;
        }
        case PROP_DO_TIMESTAMP:
        {
            if (state.active_source
                && g_object_class_find_property(G_OBJECT_GET_CLASS(state.active_source),
                                                "do-timestamp"))
            {
                g_object_get_property(G_OBJECT(state.active_source), "do-timestamp", value);
            }
            else
            {
                GST_WARNING("No active source.");
            }
            break;
        }
        case PROP_DROP_INCOMPLETE_FRAMES:
        {
            if (state.active_source
                && g_object_class_find_property(G_OBJECT_GET_CLASS(state.active_source),
                                                "drop-incomplete-buffer"))
            {
                g_object_get_property(
                    G_OBJECT(state.active_source), "drop-incomplete-buffer", value);
            }
            else
            {
                GST_WARNING("No active source.");
            }
            break;
        }
        case PROP_STATE:
        {
            if (state.active_source)
            {
                // if (state.active_source == state.main_src)
                // {
                //     g_object_get_property(G_OBJECT(state.active_source), "state", value);
                // }
                // else
                // {
                //     std::string tmp =
                //         tcam::gst::create_device_settings(state.device_serial, TCAM_PROP(self))
                //             .c_str();
                //     g_value_set_string(value, tmp.c_str());
                // }
            }
            else
            {
                GST_WARNING("No active source.");
            }
            break;
        }
        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
        }
    }
}

static void gst_tcam_src_init(GstTcamSrc* self)
{
    self->state = new tcamsrc::tcamsrc_state;

    self->state->p_monitor = gst_device_monitor_new();
    gst_device_monitor_add_filter(self->state->p_monitor, "Video/Source/tcam", nullptr);

    g_object_set(self, "message-forward", TRUE, NULL);

    self->state->pad = gst_ghost_pad_new_no_target("src", GST_PAD_SRC);
    gst_element_add_pad(GST_ELEMENT(self), self->state->pad);
}

static void gst_tcamsrc_dispose(GObject* object)
{
    GstTcamSrc* self = GST_TCAM_SRC(object);

    if (self->state->pad)
    {
        gst_element_remove_pad(GST_ELEMENT(self), self->state->pad);
        self->state->pad = nullptr;
    }
    close_active_source_element(self);
    gst_object_unref(self->state->p_monitor);
    self->state->p_monitor = nullptr;

    G_OBJECT_CLASS(GST_ELEMENT_CLASS(parent_class))->dispose(object);
}

static void gst_tcam_src_finalize(GObject* object)
{
    GstTcamSrc* self = GST_TCAM_SRC(object);

    delete self->state;
    self->state = nullptr;

    G_OBJECT_CLASS(gst_tcam_src_parent_class)->finalize(object);
}

static void gst_tcam_src_class_init(GstTcamSrcClass* klass)
{
    GST_DEBUG_CATEGORY_INIT(tcam_src_debug, "tcamsrc", 0, "tcamsrc");

    GObjectClass* gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass* element_class = GST_ELEMENT_CLASS(klass);

    gobject_class->finalize = gst_tcam_src_finalize;
    gobject_class->dispose = gst_tcamsrc_dispose;
    gobject_class->set_property = gst_tcam_src_set_property;
    gobject_class->get_property = gst_tcam_src_get_property;

    g_object_class_install_property(
        gobject_class,
        PROP_SERIAL,
        g_param_spec_string("serial",
                            "Camera serial",
                            "Serial of the camera",
                            NULL,
                            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(
        gobject_class,
        PROP_DEVICE_TYPE,
        g_param_spec_string("type",
                            "Camera type",
                            "type/backend of the camera",
                            "auto",
                            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(
        gobject_class,
        PROP_CAM_BUFFERS,
        g_param_spec_int("camera-buffers",
                         "Number of Buffers",
                         "Number of buffers to use for retrieving images",
                         1,
                         256,
                         10,
                         static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(
        gobject_class,
        PROP_NUM_BUFFERS,
        g_param_spec_int("num-buffers",
                         "Number of Buffers",
                         "Number of buffers to send before ending pipeline (-1 = unlimited)",
                         -1,
                         G_MAXINT,
                         -1,
                         static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(
        gobject_class,
        PROP_DO_TIMESTAMP,
        g_param_spec_boolean("do-timestamp",
                             "Do timestamp",
                             "Apply current stream time to buffers",
                             true,
                             static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
                                                      | G_PARAM_CONSTRUCT)));
    g_object_class_install_property(
        gobject_class,
        PROP_DROP_INCOMPLETE_FRAMES,
        g_param_spec_boolean("drop-incomplete-buffer",
                             "Drop incomplete buffers",
                             "Drop buffer that are incomplete.",
                             true,
                             static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
                                                      | G_PARAM_CONSTRUCT)));


    g_object_class_install_property(
        gobject_class,
        PROP_STATE,
        g_param_spec_string("state",
                            "Property State",
                            "Property values the internal elements shall use",
                            "",
                            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(
        gobject_class,
        PROP_TCAMDEVICE,
        g_param_spec_object("tcam-device",
                            "Tcam Device",
                            "Assigns a GstDevice to open when transitioning from NULL to READY.",
                            GST_TYPE_DEVICE,
                            static_cast<GParamFlags>(G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS)));

    gst_tcamsrc_signals[SIGNAL_DEVICE_OPEN] = g_signal_new("device-open",
                                                           G_TYPE_FROM_CLASS(klass),
                                                           G_SIGNAL_RUN_LAST,
                                                           0,
                                                           nullptr,
                                                           nullptr,
                                                           nullptr,
                                                           G_TYPE_NONE,
                                                           0,
                                                           G_TYPE_NONE);
    gst_tcamsrc_signals[SIGNAL_DEVICE_CLOSE] = g_signal_new("device-close",
                                                            G_TYPE_FROM_CLASS(klass),
                                                            G_SIGNAL_RUN_LAST,
                                                            0,
                                                            nullptr,
                                                            nullptr,
                                                            nullptr,
                                                            G_TYPE_NONE,
                                                            0,
                                                            G_TYPE_NONE);

    gst_element_class_set_static_metadata(element_class,
                                          "Tcam Video Source",
                                          "Source/Video",
                                          "Tcam based source",
                                          "The Imaging Source <support@theimagingsource.com>");

    gst_element_class_add_pad_template(element_class,
                                       gst_static_pad_template_get(&tcam_src_template));

    element_class->change_state = gst_tcam_src_change_state;
}