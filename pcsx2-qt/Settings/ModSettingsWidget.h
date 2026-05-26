// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "SettingsWidget.h"

#include "common/Pcsx2Defs.h"

#include <QtWidgets/QTableWidget>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QLabel>

#include <string>
#include <vector>

namespace GameList
{
	struct Entry;
}

class ModSettingsWidget final : public SettingsWidget
{
public:
	ModSettingsWidget(SettingsWindow* settings_dialog, const GameList::Entry* game, QWidget* parent);

private:
	void onBrowseRootClicked();
	void onRefreshClicked();
	void onAddClicked();
	void onRemoveClicked();
	void onMoveUpClicked();
	void onMoveDownClicked();
	void onTableCellChanged(int row, int column);

private:
	struct ModEntry
	{
		std::string id;
		std::string name;
		std::string version;
		std::string author;
		std::string path;
		bool enabled = false;
	};

	void loadSettings();
	void saveSettings();
	void rebuildTable();
	void scanMods();

	std::string resolveModPath(const std::string& id) const;
	std::string getModsRoot() const;
	void setModsRoot(const std::string& path);

	static ModEntry loadModMetadata(const std::string& id, const std::string& path);

	std::string m_game_path;
	bool m_updating = false;

	QLineEdit* m_root_edit = nullptr;
	QPushButton* m_root_browse_button = nullptr;
	QPushButton* m_root_open_button = nullptr;
	QTableWidget* m_table = nullptr;
	QPushButton* m_add_button = nullptr;
	QPushButton* m_remove_button = nullptr;
	QPushButton* m_up_button = nullptr;
	QPushButton* m_down_button = nullptr;
	QPushButton* m_refresh_button = nullptr;

	std::vector<ModEntry> m_mods;
};
