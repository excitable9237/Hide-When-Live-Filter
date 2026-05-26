/*
Hide When Live
Copyright (C) 2026 Jeremy

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <mutex>
#include <unordered_set>
#include <vector>

#include "hide-when-live.h"

static std::mutex g_mutex;

// Filter obs_source_t* instances (the filter source, not its parent)
static std::unordered_set<obs_source_t *> g_filter_instances;

// Scene items the plugin explicitly hid; only these are restored after transition
static std::unordered_set<obs_sceneitem_t *> g_hidden_by_plugin;

// Outgoing scene saved at transition_start with an addref; released at
// transition_video_stop.  Saving it early guards against OBS clearing
// SOURCE_A before video_stop fires.
static obs_source_t *g_outgoing_scene = nullptr;

// Transition source we are currently connected to (addref'd)
static obs_source_t *g_connected_transition = nullptr;

// Two-pass enum contexts: items are collected inside the callback, then
// obs_sceneitem_set_visible is called after enumeration exits — calling it
// inside the callback would deadlock on the scene lock that enum already holds.

struct hide_enum_ctx {
	const std::unordered_set<obs_source_t *> *filter_instances;
	std::vector<obs_sceneitem_t *> to_hide;
};

struct show_enum_ctx {
	const std::unordered_set<obs_sceneitem_t *> *hidden_set;
	std::vector<obs_sceneitem_t *> to_show;
};

static bool hide_enum_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct hide_enum_ctx *ctx = static_cast<struct hide_enum_ctx *>(param);

	if (obs_sceneitem_is_group(item)) {
		obs_scene_t *group_scene = obs_sceneitem_group_get_scene(item);
		if (group_scene)
			obs_scene_enum_items(group_scene, hide_enum_cb, param);
		return true;
	}

	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src || !obs_sceneitem_visible(item))
		return true;

	for (obs_source_t *filter : *ctx->filter_instances) {
		if (obs_filter_get_parent(filter) == src &&
		    obs_source_enabled(filter)) {
			ctx->to_hide.push_back(item);
			break;
		}
	}
	return true;
}

static bool show_enum_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct show_enum_ctx *ctx = static_cast<struct show_enum_ctx *>(param);

	if (obs_sceneitem_is_group(item)) {
		obs_scene_t *group_scene = obs_sceneitem_group_get_scene(item);
		if (group_scene)
			obs_scene_enum_items(group_scene, show_enum_cb, param);
		return true;
	}

	if (ctx->hidden_set->count(item))
		ctx->to_show.push_back(item);

	return true;
}

static void on_transition_start(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	obs_source_t *transition =
		static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!transition)
		return;

	std::lock_guard<std::mutex> lock(g_mutex);

	obs_source_release(g_outgoing_scene);
	g_outgoing_scene =
		obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_A);

	obs_source_t *incoming =
		obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_B);
	if (incoming) {
		obs_scene_t *scene = obs_scene_from_source(incoming);
		if (scene) {
			struct hide_enum_ctx ctx;
			ctx.filter_instances = &g_filter_instances;
			obs_scene_enum_items(scene, hide_enum_cb, &ctx);
			for (obs_sceneitem_t *item : ctx.to_hide) {
				obs_sceneitem_set_visible(item, false);
				g_hidden_by_plugin.insert(item);
			}
		}
		obs_source_release(incoming);
	}
}

static void on_transition_video_stop(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cd);
	std::lock_guard<std::mutex> lock(g_mutex);

	if (!g_outgoing_scene)
		return;

	obs_scene_t *scene = obs_scene_from_source(g_outgoing_scene);
	if (scene) {
		struct show_enum_ctx ctx;
		ctx.hidden_set = &g_hidden_by_plugin;
		obs_scene_enum_items(scene, show_enum_cb, &ctx);
		for (obs_sceneitem_t *item : ctx.to_show) {
			obs_sceneitem_set_visible(item, true);
			g_hidden_by_plugin.erase(item);
		}
	}

	obs_source_release(g_outgoing_scene);
	g_outgoing_scene = nullptr;
}

// signal_handler_connect/disconnect must NOT be called while holding g_mutex.
// OBS's signal dispatcher holds its own internal lock while invoking callbacks;
// our callbacks acquire g_mutex.  Holding g_mutex while calling disconnect
// (which waits on the signal lock) creates a lock-order cycle:
//   render thread: signal lock -> g_mutex
//   main thread:   g_mutex    -> signal lock  ->  deadlock

static void do_disconnect(obs_source_t *tr)
{
	signal_handler_t *sh = obs_source_get_signal_handler(tr);
	if (sh) {
		signal_handler_disconnect(sh, "transition_start",
					  on_transition_start, nullptr);
		signal_handler_disconnect(sh, "transition_video_stop",
					  on_transition_video_stop, nullptr);
	}
	obs_source_release(tr);
}

static void reconnect_transition(void)
{
	obs_source_t *old_tr = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		old_tr = g_connected_transition;
		g_connected_transition = nullptr;
	}

	if (old_tr)
		do_disconnect(old_tr);

	obs_source_t *new_tr = obs_frontend_get_current_transition();
	if (!new_tr)
		return;

	signal_handler_t *sh = obs_source_get_signal_handler(new_tr);
	if (!sh) {
		obs_source_release(new_tr);
		return;
	}

	signal_handler_connect(sh, "transition_start", on_transition_start,
			       nullptr);
	signal_handler_connect(sh, "transition_video_stop",
			       on_transition_video_stop, nullptr);

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_connected_transition = new_tr; // takes ownership of the ref
	}
}

// Called on STUDIO_MODE_DISABLED: OBS may restore item visibility on mode
// switch, so re-hide any visible items in the live scene with active filters,
// and show any stale hidden items that are no longer in the program scene.
static void reconcile_with_program_scene(void)
{
	obs_source_t *program = obs_frontend_get_current_scene();
	obs_scene_t *prog_scene =
		program ? obs_scene_from_source(program) : nullptr;

	std::lock_guard<std::mutex> lock(g_mutex);

	std::vector<obs_sceneitem_t *> to_show;
	for (obs_sceneitem_t *item : g_hidden_by_plugin) {
		if (obs_sceneitem_get_scene(item) != prog_scene)
			to_show.push_back(item);
	}
	for (obs_sceneitem_t *item : to_show) {
		obs_sceneitem_set_visible(item, true);
		g_hidden_by_plugin.erase(item);
	}

	if (prog_scene) {
		struct hide_enum_ctx ctx;
		ctx.filter_instances = &g_filter_instances;
		obs_scene_enum_items(prog_scene, hide_enum_cb, &ctx);
		for (obs_sceneitem_t *item : ctx.to_hide) {
			obs_sceneitem_set_visible(item, false);
			g_hidden_by_plugin.insert(item);
		}
	}

	if (program)
		obs_source_release(program);
}

static void on_frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_TRANSITION_CHANGED:
		reconnect_transition();
		break;

	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED: {
		// Show all plugin-hidden items so the preview reflects actual
		// scene content; transition_start will re-hide as needed.
		std::vector<obs_sceneitem_t *> to_show;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			to_show.assign(g_hidden_by_plugin.begin(),
				       g_hidden_by_plugin.end());
			g_hidden_by_plugin.clear();
		}
		for (obs_sceneitem_t *item : to_show)
			obs_sceneitem_set_visible(item, true);
		break;
	}

	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		reconcile_with_program_scene();
		break;

	case OBS_FRONTEND_EVENT_EXIT: {
		obs_source_t *old_tr = nullptr;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			old_tr = g_connected_transition;
			g_connected_transition = nullptr;
			obs_source_release(g_outgoing_scene);
			g_outgoing_scene = nullptr;
			g_hidden_by_plugin.clear();
		}
		if (old_tr)
			do_disconnect(old_tr);
		break;
	}

	default:
		break;
	}
}

static const char *hwl_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("HideWhenLive");
}

// The filter's own obs_source_t* is used as the data pointer — no allocation.
static void *hwl_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	std::lock_guard<std::mutex> lock(g_mutex);
	g_filter_instances.insert(source);
	return source;
}

static void hwl_destroy(void *data)
{
	obs_source_t *source = static_cast<obs_source_t *>(data);
	std::lock_guard<std::mutex> lock(g_mutex);
	g_filter_instances.erase(source);
}

static void hwl_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	obs_source_skip_video_filter(static_cast<obs_source_t *>(data));
}

static struct obs_source_info hwl_filter_info;

extern "C" void hwl_init(void)
{
	memset(&hwl_filter_info, 0, sizeof(hwl_filter_info));
	hwl_filter_info.id = "hwl_filter";
	hwl_filter_info.type = OBS_SOURCE_TYPE_FILTER;
	hwl_filter_info.output_flags = OBS_SOURCE_VIDEO;
	hwl_filter_info.get_name = hwl_get_name;
	hwl_filter_info.create = hwl_create;
	hwl_filter_info.destroy = hwl_destroy;
	hwl_filter_info.video_render = hwl_video_render;

	obs_register_source(&hwl_filter_info);
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
}

extern "C" void hwl_cleanup(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);

	obs_source_t *old_tr = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		old_tr = g_connected_transition;
		g_connected_transition = nullptr;
		obs_source_release(g_outgoing_scene);
		g_outgoing_scene = nullptr;
		g_hidden_by_plugin.clear();
	}
	if (old_tr)
		do_disconnect(old_tr);
}
