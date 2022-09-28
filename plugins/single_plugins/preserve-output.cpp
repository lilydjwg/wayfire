#include "wayfire/nonstd/observer_ptr.h"
#include "wayfire/object.hpp"
#include "wayfire/output.hpp"
#include "wayfire/util.hpp"
#include <sys/types.h>
#include <chrono>
#include <wayfire/plugin.hpp>
#include <wayfire/view.hpp>
#include <wayfire/core.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wayfire/util/log.hpp>
#include <wlr/util/edges.h>

#include <wayfire/plugins/common/shared-core-data.hpp>

/**
 * View last output info
 */

class last_output_info_t : public wf::custom_data_t
{
  public:
    std::string output_identifier;
    wf::geometry_t geometry;
    bool fullscreen = false;
    bool minimized  = false;
    uint32_t tiled_edges = 0;
    uint z_order;
    bool focused = false;
};

std::string make_output_identifier(wf::output_t *output)
{
    std::string identifier = "";
    identifier += output->handle->make;
    identifier += "|";
    identifier += output->handle->model;
    identifier += "|";
    identifier += output->handle->serial;
    return identifier;
}

void view_store_data(wayfire_view view, wf::output_t *output, int z_order)
{
    auto view_data = view->get_data_safe<last_output_info_t>();
    view_data->output_identifier = make_output_identifier(output);
    view_data->geometry    = view->get_wm_geometry();
    view_data->fullscreen  = view->fullscreen;
    view_data->minimized   = view->minimized;
    view_data->tiled_edges = view->tiled_edges;
    view_data->z_order     = z_order;
    if (view == output->get_active_view())
    {
        view_data->focused = true;
    }
}

nonstd::observer_ptr<last_output_info_t> view_get_data(wayfire_view view)
{
    return view->get_data<last_output_info_t>();
}

bool view_has_data(wayfire_view view)
{
    return view->has_data<last_output_info_t>();
}

void view_erase_data(wayfire_view view)
{
    view->erase_data<last_output_info_t>();
}

/**
 * Core preserve-output info
 */

wf::option_wrapper_t<int> last_output_focus_timeout{
    "preserve-output/last_output_focus_timeout"};

class preserve_output_t
{
  public:
    int instances = 0;
    std::string last_focused_output_identifier = "";
    std::chrono::time_point<std::chrono::steady_clock> last_focused_output_timestamp;

    std::map<std::string, wf::point_t> output_saved_workspace;

    preserve_output_t() = default;
    ~preserve_output_t()
    {
        LOGD("This is last instance - deleting all data");
        // Delete data from all views
        for (auto& view : wf::get_core().get_all_views())
        {
            view_erase_data(view);
        }
    }

    preserve_output_t(preserve_output_t&&) = delete;
    preserve_output_t& operator =(preserve_output_t&&) = delete;

    preserve_output_t(const preserve_output_t&) = delete;
    preserve_output_t& operator =(const preserve_output_t&) = delete;
};

/**
 * preserve-output plugin
 */

class wayfire_preserve_output : public wf::plugin_interface_t
{
    bool outputs_being_removed = false;
    wf::shared_data::ref_ptr_t<preserve_output_t> core_data;

    bool focused_output_expired()
    {
        using namespace std::chrono;
        const auto now = steady_clock::now();
        const auto last_focus_ts = core_data->last_focused_output_timestamp;
        const auto elapsed_since_focus =
            duration_cast<milliseconds>(now - last_focus_ts).count();

        return elapsed_since_focus > last_output_focus_timeout;
    }

    void store_focused_output(wf::output_t *output)
    {
        auto& last_focused_output = core_data->last_focused_output_identifier;
        // Store the output as last focused if no other output has been stored as
        // last
        // focused in the last 10 seconds
        if ((last_focused_output == "") || focused_output_expired())
        {
            LOGD("Setting last focused output to: ", output->to_string());
            last_focused_output = make_output_identifier(output);
            core_data->last_focused_output_timestamp =
                std::chrono::steady_clock::now();
        }
    }

    wf::signal_connection_t output_pre_remove = [=] (wf::signal_data_t *data)
    {
        auto signal_data = (wf::output_pre_remove_signal*)data;
        LOGD("Received pre-remove event: ", signal_data->output->to_string());
        outputs_being_removed = true;

        if (signal_data->output != output)
        {
            // This event is not for this output
            return;
        }

        // This output is being destroyed
        std::string identifier = make_output_identifier(output);

        // Store this output as the focused one
        if (wf::get_core().get_active_output() == output)
        {
            store_focused_output(output);
        }

        core_data->output_saved_workspace[identifier] =
            output->workspace->get_current_workspace();

        auto views = output->workspace->get_views_in_layer(wf::LAYER_WORKSPACE);
        for (size_t i = 0; i < views.size(); i++)
        {
            auto view = views[i];
            if ((view->role != wf::VIEW_ROLE_TOPLEVEL) || !view->is_mapped())
            {
                continue;
            }

            // Set current output and geometry in the view's last output data
            if (!view_has_data(view))
            {
                view_store_data(view, output, i);
            }
        }
    };

    wf::signal_connection_t output_removed = [=] (wf::signal_data_t *data)
    {
        auto signal_data = (wf::output_removed_signal*)data;
        LOGD("Received output-removed event: ", signal_data->output->to_string());
        outputs_being_removed = false;
    };

    void restore_views_to_output()
    {
        std::string identifier = make_output_identifier(output);

        // Restore active workspace on the output
        // We do this first so that when restoring view's geometries, they land
        // directly on the correct workspace.
        if (core_data->output_saved_workspace.count(identifier))
        {
            output->workspace->set_workspace(
                core_data->output_saved_workspace[identifier]);
        }

        // Focus this output if it was the last one focused
        if (core_data->last_focused_output_identifier == identifier)
        {
            LOGD("This is last focused output, refocusing: ", output->to_string());
            wf::get_core().focus_output(output);
            core_data->last_focused_output_identifier.clear();
        }

        // Make a list of views to move to this output
        auto views = std::vector<wayfire_view>();
        for (auto& view : wf::get_core().get_all_views())
        {
            if (!view->is_mapped())
            {
                continue;
            }

            if (!view_has_data(view))
            {
                continue;
            }

            auto last_output_info = view_get_data(view);
            if (last_output_info->output_identifier == identifier)
            {
                views.push_back(view);
            }
        }

        // Sorts with the views closest to front last
        std::sort(views.begin(), views.end(),
            [=] (wayfire_view & view1, wayfire_view & view2)
        {
            return view_get_data(view1)->z_order > view_get_data(view2)->z_order;
        });

        // Move views to this output
        for (auto& view : views)
        {
            auto last_output_info = view_get_data(view);
            LOGD("Restoring view: ",
                view->get_title(), " to: ", output->to_string());

            wf::get_core().move_view_to_output(view, output, false);
            view->set_fullscreen(last_output_info->fullscreen);
            view->set_minimized(last_output_info->minimized);
            if (last_output_info->tiled_edges != 0)
            {
                view->tile_request(last_output_info->tiled_edges);
            }

            view->set_geometry(last_output_info->geometry);

            // Focus
            if (last_output_info->focused)
            {
                LOGD("Focusing view: ", view->get_title());
                output->focus_view(view, false);
            }

            // Z Order
            output->workspace->bring_to_front(view);

            // Remove all last output info from views
            view_erase_data(view);
        }
    }

    wf::wl_idle_call idle_restore_views;

  public:
    void init() override
    {
        if (wlr_output_is_headless(output->handle))
        {
            // Don't do anything for NO-OP outputs
            return;
        }

        // Call restore_views_to_output() after returning to main loop
        idle_restore_views.run_once([=] ()
        {
            restore_views_to_output();
        });

        wf::get_core().output_layout->connect_signal("output-pre-remove",
            &output_pre_remove);
        wf::get_core().output_layout->connect_signal("output-removed",
            &output_removed);
    }

    void fini() override
    {
        // Nothing to do
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_preserve_output);
