#include "wayfire/core.hpp"
#include "wayfire/geometry.hpp"
#include "wayfire/object.hpp"
#include <wayfire/window-manager.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/toplevel.hpp>
#include <wayfire/output.hpp>
#include <wayfire/workarea.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/txn/transaction-manager.hpp>

namespace wf
{
class windowed_geometry_data_t : public wf::custom_data_t
{
  public:
    /** Last geometry the view has had in non-tiled and non-fullscreen state.
     * -1 as width/height means that no such geometry has been stored. */
    wf::geometry_t last_windowed_geometry = {0, 0, -1, -1};

    /**
     * The workarea when last_windowed_geometry was stored. This is used
     * for ex. when untiling a view to determine its geometry relative to the
     * (potentially changed) workarea of its output.
     */
    wf::geometry_t windowed_geometry_workarea = {0, 0, -1, -1};
};

void wf::window_manager_t::update_last_windowed_geometry(wayfire_toplevel_view view)
{
    if (!view->is_mapped() || view->pending_tiled_edges() || view->fullscreen)
    {
        return;
    }

    auto windowed = view->get_data_safe<windowed_geometry_data_t>();

    windowed->last_windowed_geometry = view->toplevel()->pending().geometry;
    if (view->get_output())
    {
        windowed->windowed_geometry_workarea = view->get_output()->workarea->get_workarea();
    } else
    {
        windowed->windowed_geometry_workarea = {0, 0, -1, -1};
    }
}

std::optional<wf::geometry_t> wf::window_manager_t::get_last_windowed_geometry(wayfire_toplevel_view view)
{
    auto windowed = view->get_data_safe<windowed_geometry_data_t>();

    if ((windowed->windowed_geometry_workarea.width <= 0) || (windowed->last_windowed_geometry.width <= 0))
    {
        return {};
    }

    if (!view->get_output())
    {
        return windowed->last_windowed_geometry;
    }

    const auto& geom     = windowed->last_windowed_geometry;
    const auto& old_area = windowed->windowed_geometry_workarea;
    const auto& new_area = view->get_output()->workarea->get_workarea();
    return wf::geometry_t{
        .x     = new_area.x + (geom.x - old_area.x) * new_area.width / old_area.width,
        .y     = new_area.y + (geom.y - old_area.y) * new_area.height / old_area.height,
        .width = geom.width * new_area.width / old_area.width,
        .height = geom.height * new_area.height / old_area.height
    };
}

void window_manager_t::move_request(wayfire_toplevel_view view)
{
    if (view->get_output())
    {
        view_move_request_signal data;
        data.view = view;
        view->get_output()->emit(&data);
    }
}

void window_manager_t::resize_request(wayfire_toplevel_view view, uint32_t edges)
{
    if (view->get_output())
    {
        view_resize_request_signal data;
        data.view  = view;
        data.edges = edges;
        view->get_output()->emit(&data);
    }
}

void window_manager_t::focus_request(wayfire_toplevel_view view)
{
    if (view->get_output())
    {
        view_focus_request_signal data;
        data.view = view;
        data.self_request = false;

        view->emit(&data);
        wf::get_core().emit(&data);
        if (!data.carried_out)
        {
            wf::get_core().focus_output(view->get_output());
            view->get_output()->ensure_visible(view);
            view->get_output()->focus_view(view, true);
        }
    }
}

void window_manager_t::minimize_request(wayfire_toplevel_view view, bool minimized)
{
    if ((view->minimized == minimized) || !view->is_mapped())
    {
        return;
    }

    view_minimize_request_signal data;
    data.view  = view;
    data.state = minimized;

    if (view->get_output())
    {
        view->get_output()->emit(&data);
    }

    /* Some plugin (e.g animate) will take care of the request, so we need
     * to just send proper state to foreign-toplevel clients */
    if (data.carried_out)
    {
        view->minimized = minimized;
        if (view->get_output())
        {
            view->get_output()->refocus();
        }
    } else
    {
        /* Do the default minimization */
        view->set_minimized(minimized);
    }
}

/**
 * Put a view on the given workspace.
 */
static void move_to_workspace(wayfire_toplevel_view view, wf::point_t workspace)
{
    auto output = view->get_output();
    auto wm_geometry = view->get_wm_geometry();
    auto delta    = workspace - output->wset()->get_current_workspace();
    auto scr_size = output->get_screen_size();

    wm_geometry.x += scr_size.width * delta.x;
    wm_geometry.y += scr_size.height * delta.y;
    view->move(wm_geometry.x, wm_geometry.y);
}

void window_manager_t::tile_request(wayfire_toplevel_view view,
    uint32_t tiled_edges, std::optional<wf::point_t> ws)
{
    if (view->fullscreen || !view->get_output())
    {
        return;
    }

    const wf::point_t workspace = ws.value_or(view->get_output()->wset()->get_current_workspace());

    view_tile_request_signal data;
    data.view  = view;
    data.edges = tiled_edges;
    data.workspace    = workspace;
    data.desired_size = tiled_edges ? view->get_output()->workarea->get_workarea() :
        get_last_windowed_geometry(view).value_or(wf::geometry_t{0, 0, -1, -1});

    update_last_windowed_geometry(view);
    view->toplevel()->pending().tiled_edges = tiled_edges;
    if (view->is_mapped())
    {
        view->get_output()->emit(&data);
    }

    if (!data.carried_out)
    {
        if (data.desired_size.width > 0)
        {
            view->set_geometry(data.desired_size);
        } else
        {
            view->request_native_size();
        }

        wf::get_core().tx_manager->schedule_object(view->toplevel());
        move_to_workspace(view, workspace);
    }
}

void window_manager_t::fullscreen_request(wayfire_toplevel_view view,
    wf::output_t *output, bool state, std::optional<wf::point_t> ws)
{
    wf::output_t *wo = output ?: (view->get_output() ?: wf::get_core().get_active_output());
    const wf::point_t workspace = ws.value_or(wo->wset()->get_current_workspace());
    wf::dassert(wo != nullptr, "Fullscreening should not happen with null output!");

    /* TODO: what happens if the view is moved to the other output, but not
     * fullscreened? We should make sure that it stays visible there */
    if (view->get_output() != wo)
    {
        // TODO: move_view_to_output seems like a good candidate for inclusion in window-manager
        wf::move_view_to_output(view, wo, false);
    }

    view_fullscreen_request_signal data;
    data.view  = view;
    data.state = state;
    data.workspace    = workspace;
    data.desired_size = wo->get_relative_geometry();

    if (!state)
    {
        data.desired_size = view->pending_tiled_edges() ? wo->workarea->get_workarea() :
            get_last_windowed_geometry(view).value_or(wf::geometry_t{0, 0, -1, -1});
    }

    view->set_fullscreen(state);
    if (view->is_mapped())
    {
        wo->emit(&data);
    }

    if (!data.carried_out)
    {
        if (data.desired_size.width > 0)
        {
            view->set_geometry(data.desired_size);
        } else
        {
            view->request_native_size();
        }

        move_to_workspace(view, workspace);
    }
}
} // namespace wf