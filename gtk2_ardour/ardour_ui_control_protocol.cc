/*
 * Copyright (C) 2026 NaxeCode
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* Requests from control surfaces (ControlProtocol::GuiRequest) for what only
 * the GUI can do: set the audio engine through its window, see and answer
 * open dialogs, and see and move the editor's view. Each takes its
 * arguments as JSON and answers with a JSON object, {"error": "..."} when it
 * can't.
 */

#include <cstdio>
#include <sstream>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <ytkmm/button.h>
#include <ytkmm/dialog.h>
#include <ytkmm/label.h>
#include <ytkmm/messagedialog.h>
#include <ytkmm/window.h>

#include <glibmm/main.h>

#include "pbd/id.h"

#include "ardour/audioengine.h"
#include "ardour/route.h"
#include "ardour/session.h"

#include "gtkmm2ext/actions.h"

#include "ardour_ui.h"
#include "engine_dialog.h"
#include "public_editor.h"
#include "enums.h"
#include "time_axis_view.h"
#include "track_view_list.h"

using namespace ARDOUR;

namespace pt = boost::property_tree;

namespace {

std::string
json_text (std::string const& s)
{
	std::ostringstream out;
	out << '"';
	for (std::string::const_iterator c = s.begin (); c != s.end (); ++c) {
		switch (*c) {
			case '"':
				out << "\\\"";
				break;
			case '\\':
				out << "\\\\";
				break;
			case '\n':
				out << "\\n";
				break;
			case '\r':
				out << "\\r";
				break;
			case '\t':
				out << "\\t";
				break;
			default:
				if (static_cast<unsigned char> (*c) < 0x20) {
					char buf[8];
					snprintf (buf, sizeof (buf), "\\u%04x", static_cast<unsigned char> (*c));
					out << buf;
				} else {
					out << *c;
				}
		}
	}
	out << '"';
	return out.str ();
}

std::string
error_json (std::string const& message)
{
	return "{\"error\":" + json_text (message) + "}";
}

/* A button's label as a person reads it: without the mnemonic underscore. */
std::string
plain_label (std::string label)
{
	std::string::size_type u;
	while ((u = label.find ('_')) != std::string::npos) {
		label.erase (u, 1);
	}
	return label;
}

void
collect_labels (Gtk::Widget* w, std::vector<std::string>& texts)
{
	if (Gtk::Label* l = dynamic_cast<Gtk::Label*> (w)) {
		if (!l->get_text ().empty ()) {
			texts.push_back (l->get_text ());
		}
		return;
	}
	if (Gtk::Container* c = dynamic_cast<Gtk::Container*> (w)) {
		std::list<Gtk::Widget*> children = c->get_children ();
		for (std::list<Gtk::Widget*>::iterator i = children.begin (); i != children.end (); ++i) {
			collect_labels (*i, texts);
		}
	}
}

/* The dialogs a person sees now, topmost last. */
std::vector<Gtk::Dialog*>
open_dialogs ()
{
	std::vector<Gtk::Dialog*> dialogs;
	std::list<Gtk::Window*>   windows = Gtk::Window::list_toplevels ();
	for (std::list<Gtk::Window*>::iterator w = windows.begin (); w != windows.end (); ++w) {
		Gtk::Dialog* d = dynamic_cast<Gtk::Dialog*> (*w);
		if (d && d->get_visible ()) {
			dialogs.push_back (d);
		}
	}
	return dialogs;
}

std::vector<Gtk::Button*>
dialog_buttons (Gtk::Dialog* d)
{
	std::vector<Gtk::Button*> buttons;
	std::list<Gtk::Widget*> children = d->get_action_area ()->get_children ();
	for (std::list<Gtk::Widget*>::iterator i = children.begin (); i != children.end (); ++i) {
		Gtk::Button* b = dynamic_cast<Gtk::Button*> (*i);
		if (b && b->get_visible ()) {
			buttons.push_back (b);
		}
	}
	return buttons;
}

std::string
dialog_json (Gtk::Dialog* d, size_t index)
{
	std::vector<std::string> texts;
	if (Gtk::MessageDialog* m = dynamic_cast<Gtk::MessageDialog*> (d)) {
		gchar* text      = 0;
		gchar* secondary = 0;
		g_object_get (G_OBJECT (m->gobj ()), "text", &text, "secondary-text", &secondary, NULL);
		if (text && *text) {
			texts.push_back (text);
		}
		if (secondary && *secondary) {
			texts.push_back (secondary);
		}
		g_free (text);
		g_free (secondary);
	}
	if (texts.empty ()) {
		collect_labels (d->get_vbox (), texts);
	}

	std::ostringstream out;
	out << "{\"index\":" << index << ",\"title\":" << json_text (d->get_title ())
	    << ",\"modal\":" << (d->get_modal () ? "true" : "false") << ",\"text\":[";
	for (size_t i = 0; i < texts.size (); ++i) {
		out << (i ? "," : "") << json_text (texts[i]);
	}
	out << "],\"buttons\":[";
	std::vector<Gtk::Button*> buttons = dialog_buttons (d);
	for (size_t i = 0; i < buttons.size (); ++i) {
		out << (i ? "," : "") << json_text (plain_label (buttons[i]->get_label ()));
	}
	out << "]}";
	return out.str ();
}

} // namespace

std::string
ARDOUR_UI::control_protocol_gui_request (std::string request, std::string arguments)
{
	pt::ptree args;
	try {
		std::istringstream in (arguments.empty () ? std::string ("{}") : arguments);
		pt::read_json (in, args);
	} catch (...) {
		return error_json ("arguments are not JSON");
	}

	if (request == "engine/set") {
		EngineControl* ec = dynamic_cast<EngineControl*> (audio_midi_setup.get (true));
		if (!ec) {
			return error_json ("the Audio/MIDI Setup window is not available");
		}
		/* the caller hears about a sample rate mismatch in the answer */
		_quiet_sr_mismatch = true;
		Glib::signal_timeout ().connect_once ([this] () { _quiet_sr_mismatch = false; }, 3000);
		std::string const error = ec->configure_for_control_protocol (
		    args.get<std::string> ("backend", ""), args.get<std::string> ("device", ""),
		    args.get<std::string> ("inputDevice", ""), args.get<std::string> ("outputDevice", ""),
		    args.get<float> ("sampleRate", 0), args.get<uint32_t> ("bufferSize", 0));
		if (!error.empty ()) {
			return error_json (error);
		}
		std::ostringstream out;
		out << "{\"running\":" << (AudioEngine::instance ()->running () ? "true" : "false")
		    << ",\"sampleRate\":" << AudioEngine::instance ()->sample_rate ();
		if (_session) {
			out << ",\"sessionSampleRate\":" << _session->nominal_sample_rate ()
			    << ",\"resampled\":" << (_session->nominal_sample_rate () != AudioEngine::instance ()->sample_rate () ? "true" : "false");
		}
		out << "}";
		return out.str ();
	}

	if (request == "ui/dialogs_list") {
		std::vector<Gtk::Dialog*> dialogs = open_dialogs ();
		std::ostringstream        out;
		out << "{\"dialogs\":[";
		for (size_t i = 0; i < dialogs.size (); ++i) {
			out << (i ? "," : "") << dialog_json (dialogs[i], i);
		}
		out << "]}";
		return out.str ();
	}

	if (request == "ui/dialog_respond") {
		std::vector<Gtk::Dialog*> dialogs = open_dialogs ();
		std::string const         title   = args.get<std::string> ("title", "");
		std::string const         button  = args.get<std::string> ("button", "");
		Gtk::Dialog*              dialog  = 0;
		if (args.count ("index")) {
			size_t const i = args.get<size_t> ("index", 0);
			if (i < dialogs.size ()) {
				dialog = dialogs[i];
			}
		} else if (!title.empty ()) {
			for (std::vector<Gtk::Dialog*>::iterator d = dialogs.begin (); d != dialogs.end (); ++d) {
				if ((*d)->get_title () == title) {
					dialog = *d;
				}
			}
		} else if (dialogs.size () == 1) {
			dialog = dialogs.front ();
		}
		if (!dialog) {
			return error_json (dialogs.empty () ? "no dialog is open" : "say which dialog: index or title (ui_dialogs_list)");
		}
		std::vector<Gtk::Button*> buttons = dialog_buttons (dialog);
		for (std::vector<Gtk::Button*>::iterator b = buttons.begin (); b != buttons.end (); ++b) {
			if (plain_label ((*b)->get_label ()) != button) {
				continue;
			}
			std::string const answered = dialog->get_title ();
			int const         response = gtk_dialog_get_response_for_widget (dialog->gobj (), GTK_WIDGET ((*b)->gobj ()));
			if (response != GTK_RESPONSE_NONE) {
				dialog->response (response);
			} else {
				(*b)->clicked ();
			}
			return "{\"answered\":" + json_text (answered) + ",\"button\":" + json_text (button) + "}";
		}
		return error_json ("no button \"" + button + "\" in \"" + dialog->get_title () + "\"");
	}

	if (request == "ui/run_action") {
		std::string const path = args.get<std::string> ("action", "");
		Glib::RefPtr<Gtk::Action> act = ActionManager::get_action (path, false);
		if (!act) {
			return error_json ("no action " + path + " (as Group/name, e.g. Editor/zoom-to-session)");
		}
		act->activate ();
		return "{\"ran\":" + json_text (path) + "}";
	}

	if (request.compare (0, 7, "editor/") != 0) {
		return error_json ("unknown request " + request);
	}
	if (!editor || !_session) {
		return error_json ("no session is open in the editor");
	}

	TimeAxisView* tv = 0;
	if (args.count ("trackId") || args.count ("trackName")) {
		std::shared_ptr<Route> route;
		if (args.count ("trackId")) {
			route = _session->route_by_id (PBD::ID (args.get<std::string> ("trackId", "")));
		} else {
			route = _session->route_by_name (args.get<std::string> ("trackName", ""));
		}
		if (!route) {
			return error_json ("track not found");
		}
		tv = editor->time_axis_view_from_stripable (route);
		if (!tv) {
			return error_json ("the editor has no view of that track");
		}
	}

	if (request == "editor/get_view") {
		double const      top    = editor->get_y_origin ();
		double const      height = editor->visible_canvas_height ();
		std::ostringstream out;
		out << "{\"leftmostSample\":" << editor->leftmost_sample ()
		    << ",\"pageSamples\":" << editor->current_page_samples ()
		    << ",\"samplesPerPixel\":" << editor->get_current_zoom ()
		    << ",\"yOrigin\":" << top << ",\"visibleHeight\":" << height << ",\"tracks\":[";
		TrackViewList const& views = editor->get_track_views ();
		bool                 first = true;
		for (TrackViewList::const_iterator v = views.begin (); v != views.end (); ++v) {
			std::shared_ptr<Stripable> s = (*v)->stripable ();
			if (!s) {
				continue;
			}
			bool const shown   = !(*v)->hidden ();
			double const y     = (*v)->y_position ();
			double const h     = (*v)->current_height ();
			bool const on_page = shown && y >= 0 && y + h > top && y < top + height;
			out << (first ? "" : ",") << "{\"id\":" << json_text (s->id ().to_s ()) << ",\"name\":" << json_text (s->name ())
			    << ",\"shown\":" << (shown ? "true" : "false") << ",\"height\":" << h << ",\"y\":" << y
			    << ",\"onScreen\":" << (on_page ? "true" : "false") << "}";
			first = false;
		}
		out << "]}";
		return out.str ();
	}

	if (request == "editor/zoom_to_session") {
		Glib::RefPtr<Gtk::Action> act = ActionManager::get_action ("Editor", "zoom-to-session", false);
		if (!act) {
			return error_json ("no zoom-to-session action");
		}
		act->activate ();
		return "{\"zoomed\":true}";
	}

	if (request == "editor/scroll_to_time") {
		samplepos_t const sample = args.get<samplepos_t> ("sample", -1);
		if (sample < 0) {
			return error_json ("missing sample");
		}
		editor->reset_x_origin (sample);
		return "{\"leftmostSample\":" + std::to_string (sample) + "}";
	}

	if (!tv) {
		return error_json ("missing trackId or trackName");
	}

	if (request == "editor/scroll_to_track") {
		editor->ensure_time_axis_view_is_visible (*tv, args.get<bool> ("atTop", true));
		return "{\"scrolledTo\":" + json_text (tv->name ()) + "}";
	}

	if (request == "editor/set_track_visible") {
		bool const visible = args.get<bool> ("visible", true);
		if (visible) {
			editor->show_track_in_display (tv, false);
		} else {
			editor->hide_track_in_display (tv, false);
		}
		return "{\"track\":" + json_text (tv->name ()) + ",\"shown\":" + (visible ? "true" : "false") + "}";
	}

	if (request == "editor/set_track_height") {
		std::string const preset = args.get<std::string> ("preset", "");
		uint32_t          height = args.get<uint32_t> ("height", 0);
		if (!preset.empty ()) {
			Height h;
			if (preset == "largest") {
				h = HeightLargest;
			} else if (preset == "larger") {
				h = HeightLarger;
			} else if (preset == "large") {
				h = HeightLarge;
			} else if (preset == "normal") {
				h = HeightNormal;
			} else if (preset == "small") {
				h = HeightSmall;
			} else {
				return error_json ("preset is one of largest, larger, large, normal, small");
			}
			height = TimeAxisView::preset_height (h);
		}
		if (height == 0) {
			return error_json ("missing height (pixels) or preset");
		}
		tv->set_height (height);
		return "{\"track\":" + json_text (tv->name ()) + ",\"height\":" + std::to_string (tv->current_height ()) + "}";
	}

	return error_json ("unknown request " + request);
}
