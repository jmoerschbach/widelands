/*
 * Copyright (C) 2002 by the Widelands Development Team
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include "fullscreen_menu_launchgame.h"
#include "fullscreen_menu_mapselect.h"
#include "game.h"
#include "i18n.h"
#include "instances.h"
#include "player.h"
#include "network.h"
#include "map.h"
#include "playerdescrgroup.h"
#include "ui_button.h"
#include "ui_textarea.h"

/*
==============================================================================

Fullscreen_Menu_LaunchGame

==============================================================================
*/

Fullscreen_Menu_LaunchGame::Fullscreen_Menu_LaunchGame(Game *g, NetGame* ng, Map_Loader** ml)
	: Fullscreen_Menu_Base("launchgamemenu.jpg")
{
	m_game = g;
	m_netgame = ng;
	m_is_scenario = false;
	m_ml=ml;

	// Title
	UITextarea* title= new UITextarea(this, MENU_XRES/2, 120, _("Launch Game"), Align_HCenter);
	title->set_font(UI_FONT_BIG, UI_FONT_CLR_FG);

	// UIButtons
	UIButton* b;

	b = new UIButton(this, 550, 450, 200, 26, 0, 0);
	b->clicked.set(this, &Fullscreen_Menu_LaunchGame::back_clicked);
	b->set_title(_("Back").c_str());

	m_ok = new UIButton(this, 550, 480, 200, 26, 2, 1);
	m_ok->clicked.set(this, &Fullscreen_Menu_LaunchGame::start_clicked);
	m_ok->set_title(_("Start game").c_str());
	m_ok->set_enabled(false);
	m_ok->set_visible(m_netgame==0 || m_netgame->is_host());

	// Map selection fields
	m_mapname = new UITextarea(this, 650, 250, "(no map)", Align_HCenter);
	b = new UIButton(this, 550, 280, 200, 26, 1, 0);
	b->clicked.set(this, &Fullscreen_Menu_LaunchGame::select_map);
	b->set_title(_("Select map").c_str());
	b->set_enabled (m_netgame==0 || m_netgame->is_host());

	// Player settings
	int i;
	int y;

	y = 250;
	for(i = 1; i <= MAX_PLAYERS; i++)	{ // players start with 1, not 0
		PlayerDescriptionGroup *pdg = new PlayerDescriptionGroup(this, 50, y, m_game, i, m_netgame && m_netgame->get_playernum()==i);
		pdg->changed.set(this, &Fullscreen_Menu_LaunchGame::refresh);

		m_players[i-1] = pdg;
		y += 30;

		if (m_netgame!=0)
		    m_netgame->set_player_description_group (i, pdg);
	}

	if (m_netgame==0)
		m_players[0]->set_player_type (Player::playerLocal);

	// Directly go selecting a map
	if (m_netgame==0 || m_netgame->is_host())
		select_map();
}

void Fullscreen_Menu_LaunchGame::think()
{
	m_game->think();
}

/*
 * back-button has been pressed
 * */
void Fullscreen_Menu_LaunchGame::back_clicked()
{
	m_game->get_objects()->cleanup(m_game);
	end_modal(0);
}


/*
 * start-button has been pressed
 * */
void Fullscreen_Menu_LaunchGame::start_clicked()
{
	end_modal(m_is_scenario?2:1);
}


void Fullscreen_Menu_LaunchGame::refresh()
{
	Map* map = m_game->get_map();
	int maxplayers = 0;

	// update the mapname
	if (map)
	{
		m_mapname->set_text(map->get_name());
		maxplayers = map->get_nrplayers();
	}
	else
		m_mapname->set_text(_("(no map)"));

	// update the player description groups
	for(int i = 0; i < MAX_PLAYERS; i++) {
		m_players[i]->allow_changes(PlayerDescriptionGroup::CHANGE_EVERYTHING);
		m_players[i]->set_enabled(i < maxplayers);
		if(m_is_scenario && (i<maxplayers) && map) {
			// set player to the by the map given
			m_players[i]->allow_changes(PlayerDescriptionGroup::CHANGE_NOTHING);
			m_players[i]->set_player_tribe(map->get_scenario_player_tribe(i+1));
			m_players[i]->set_player_name(map->get_scenario_player_name(i+1));
		} else if(i<maxplayers && map ) {
			std::string name=_("Player ");
			if((i+1)/10) name.append(1, static_cast<char>((i+1)/10 + 0x30));
			name.append(1, static_cast<char>(((i+1)%10) + 0x30));
			m_players[i]->set_player_name(name);
			m_players[i]->allow_changes(PlayerDescriptionGroup::CHANGE_EVERYTHING);
		}

		if (m_netgame!=0) {
			int allow=PlayerDescriptionGroup::CHANGE_NOTHING;

			if (m_netgame->is_host() && i>0)
				allow|=PlayerDescriptionGroup::CHANGE_ENABLED;

			if (m_netgame->get_playernum()==i+1)
				allow|=PlayerDescriptionGroup::CHANGE_TRIBE;

			m_players[i]->allow_changes ((PlayerDescriptionGroup::changemode_t) allow);
		}
	}

	m_ok->set_enabled(m_game->can_start());
}

void Fullscreen_Menu_LaunchGame::select_map()
{
	Fullscreen_Menu_MapSelect* msm=new Fullscreen_Menu_MapSelect(m_game, m_ml);
	if(msm->run()==2)
		m_is_scenario=true;
	else
		m_is_scenario=false;

	delete msm;

	if (m_netgame)
		static_cast<NetHost*>(m_netgame)->update_map();

	refresh();
}
