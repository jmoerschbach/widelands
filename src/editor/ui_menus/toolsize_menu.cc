/*
 * Copyright (C) 2002-2021 by the Widelands Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include "editor/ui_menus/toolsize_menu.h"

#include "base/i18n.h"
#include "editor/editorinteractive.h"
#include "editor/tools/tool.h"

inline EditorInteractive& EditorToolsizeMenu::eia() const {
	return dynamic_cast<EditorInteractive&>(*get_parent());
}

/**
 * Create all the buttons etc...
 */
EditorToolsizeMenu::EditorToolsizeMenu(EditorInteractive& parent,
                                       UI::UniqueWindow::Registry& registry)
   : UI::UniqueWindow(
        &parent, UI::WindowStyle::kWui, "toolsize_menu", &registry, 250, 50, _("Tool Size")),
     box_horizontal_(this, UI::PanelStyle::kFsMenu, 0, 0, UI::Box::Horizontal),
     box_vertical_(&box_horizontal_, UI::PanelStyle::kFsMenu, 0, 0, UI::Box::Vertical),
     sb_toolsize_(&box_vertical_,
                  0,
                  0,
                  200,
                  80,
                  1,
                  1,
                  MAX_TOOL_AREA + 1,
                  UI::PanelStyle::kFsMenu,
                  _("Current Size:")),

     value_(0) {

	box_horizontal_.add(&box_vertical_, UI::Box::Resizing::kAlign, UI::Align::kCenter);
	box_vertical_.add(&sb_toolsize_, UI::Box::Resizing::kAlign, UI::Align::kCenter);
	box_horizontal_.set_size(get_inner_w(), get_inner_h());
	box_vertical_.set_size(get_inner_w(), get_inner_h());
	update(parent.get_sel_radius());

	if (eia().tools()->current().has_size_one()) {
		set_buttons_enabled(false);
	}

	if (get_usedefaultpos()) {
		center_to_parent();
	}
	sb_toolsize_.changed.connect([this]() { size_changed(); });
	initialization_complete();
}

void EditorToolsizeMenu::update(uint32_t const val) {
	sb_toolsize_.set_value(val);
}

void EditorToolsizeMenu::size_changed() {
	value_ = sb_toolsize_.get_value() - 1;
	eia().set_sel_radius(value_);
}
void EditorToolsizeMenu::set_buttons_enabled(bool enabled) {
	if (enabled) {
		sb_toolsize_.set_interval(1, MAX_TOOL_AREA + 1);
	} else {
		sb_toolsize_.set_interval(1, 1);
	}
}
