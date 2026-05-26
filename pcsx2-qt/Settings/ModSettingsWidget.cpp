// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Settings/ModSettingsWidget.h"

#include "SettingsWindow.h"
#include "QtUtils.h"

#include "pcsx2/GameList.h"
#include "pcsx2/INISettingsInterface.h"

#include "common/FileSystem.h"
#include "common/HeterogeneousContainers.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>
#include <QtCore/QDir>
#include <QtCore/QUrl>

ModSettingsWidget::ModSettingsWidget(SettingsWindow* settings_dialog, const GameList::Entry* game, QWidget* parent)
	: SettingsWidget(settings_dialog, parent)
{
	if (game)
		m_game_path = game->path;

	QWidget* contents = new QWidget(this);
	QVBoxLayout* layout = new QVBoxLayout(contents);

	{
		QHBoxLayout* root_layout = new QHBoxLayout();
		QLabel* label = new QLabel(tr("Mod Folder:"), contents);
		m_root_edit = new QLineEdit(contents);
		m_root_browse_button = new QPushButton(tr("Browse..."), contents);
		m_root_open_button = new QPushButton(tr("Open"), contents);

		root_layout->addWidget(label);
		root_layout->addWidget(m_root_edit, 1);
		root_layout->addWidget(m_root_browse_button);
		root_layout->addWidget(m_root_open_button);
		layout->addLayout(root_layout);
	}

	m_table = new QTableWidget(contents);
	m_table->setColumnCount(5);
	m_table->setHorizontalHeaderLabels({tr("Enabled"), tr("Name"), tr("Version"), tr("Author"), tr("Path")});
	m_table->horizontalHeader()->setStretchLastSection(true);
	m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setSelectionMode(QAbstractItemView::SingleSelection);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	layout->addWidget(m_table, 1);

	{
		QHBoxLayout* buttons = new QHBoxLayout();
		m_add_button = new QPushButton(tr("Add Mod"), contents);
		m_remove_button = new QPushButton(tr("Remove"), contents);
		m_up_button = new QPushButton(tr("Move Up"), contents);
		m_down_button = new QPushButton(tr("Move Down"), contents);
		m_refresh_button = new QPushButton(tr("Refresh"), contents);

		buttons->addWidget(m_add_button);
		buttons->addWidget(m_remove_button);
		buttons->addWidget(m_up_button);
		buttons->addWidget(m_down_button);
		buttons->addStretch(1);
		buttons->addWidget(m_refresh_button);
		layout->addLayout(buttons);
	}

	connect(m_root_browse_button, &QPushButton::clicked, this, &ModSettingsWidget::onBrowseRootClicked);
	connect(m_root_open_button, &QPushButton::clicked, this, [this]() {
		const QString path = m_root_edit->text();
		if (!path.isEmpty())
			QtUtils::OpenURL(this, QUrl::fromLocalFile(path));
	});
	connect(m_refresh_button, &QPushButton::clicked, this, &ModSettingsWidget::onRefreshClicked);
	connect(m_add_button, &QPushButton::clicked, this, &ModSettingsWidget::onAddClicked);
	connect(m_remove_button, &QPushButton::clicked, this, &ModSettingsWidget::onRemoveClicked);
	connect(m_up_button, &QPushButton::clicked, this, &ModSettingsWidget::onMoveUpClicked);
	connect(m_down_button, &QPushButton::clicked, this, &ModSettingsWidget::onMoveDownClicked);
	connect(m_table, &QTableWidget::cellChanged, this, &ModSettingsWidget::onTableCellChanged);

	addTab(QString(), contents);
	loadSettings();
}

std::string ModSettingsWidget::getModsRoot() const
{
	auto* sif = dialog()->getSettingsInterface();
	std::string root;
	if (sif)
		root = sif->GetStringValue("Mods", "Root", "");

	if (!root.empty() && !FileSystem::DirectoryExists(root.c_str()) && !m_game_path.empty())
	{
		const std::string fallback_lower = Path::Combine(m_game_path, "mods");
		const std::string fallback_upper = Path::Combine(m_game_path, "Mods");
		if (FileSystem::DirectoryExists(fallback_lower.c_str()))
			root = fallback_lower;
		else if (FileSystem::DirectoryExists(fallback_upper.c_str()))
			root = fallback_upper;
	}

	if (root.empty() && !m_game_path.empty())
	{
		root = Path::Combine(m_game_path, "mods");
		if (!FileSystem::DirectoryExists(root.c_str()))
		{
			const std::string alt_root = Path::Combine(m_game_path, "Mods");
			if (FileSystem::DirectoryExists(alt_root.c_str()))
				root = alt_root;
		}
	}

	return root;
}

void ModSettingsWidget::setModsRoot(const std::string& path)
{
	auto* sif = dialog()->getSettingsInterface();
	if (!sif)
		return;

	sif->SetStringValue("Mods", "Root", path.c_str());
	dialog()->saveAndReloadGameSettings();
}

std::string ModSettingsWidget::resolveModPath(const std::string& id) const
{
	if (Path::IsAbsolute(id))
		return id;

	const std::string root = getModsRoot();
	if (root.empty())
		return id;

	return Path::Combine(root, id);
}

ModSettingsWidget::ModEntry ModSettingsWidget::loadModMetadata(const std::string& id, const std::string& path)
{
	ModEntry entry;
	entry.id = id;
	entry.path = path;

	const std::string ini_path = Path::Combine(path, "mod.ini");
	if (FileSystem::FileExists(ini_path.c_str()))
	{
		INISettingsInterface ini(ini_path);
		if (ini.Load())
		{
			entry.name = ini.GetStringValue("Mod", "Name", "");
			entry.author = ini.GetStringValue("Mod", "Author", "");
			entry.version = ini.GetStringValue("Mod", "Version", "");
		}
	}

	if (entry.name.empty())
		entry.name = Path::GetFileName(path);

	return entry;
}

void ModSettingsWidget::scanMods()
{
	m_mods.clear();

	auto* sif = dialog()->getSettingsInterface();
	if (!sif)
		return;

	const std::string root = getModsRoot();
	const std::vector<std::string> order = sif->GetStringList("Mods", "Order");
	const std::vector<std::string> enabled = sif->GetStringList("Mods", "Enabled");

	UnorderedStringMap<std::string> enabled_map;
	for (const std::string& id : enabled)
		enabled_map[id] = id;

	UnorderedStringMap<std::string> seen;

	// Pull mods from order list first
	for (const std::string& id : order)
	{
		const std::string mod_path = resolveModPath(id);
		if (!FileSystem::DirectoryExists(mod_path.c_str()))
			continue;

		ModEntry entry = loadModMetadata(id, mod_path);
		entry.enabled = enabled_map.find(id) != enabled_map.end();
		m_mods.push_back(std::move(entry));
		seen[id] = id;
	}

	// Scan root for any new mods
	if (!root.empty() && FileSystem::DirectoryExists(root.c_str()))
	{
		FileSystem::FindResultsArray folders;
		FileSystem::FindFiles(root.c_str(), "*", FILESYSTEM_FIND_FOLDERS, &folders);
		for (const FILESYSTEM_FIND_DATA& fd : folders)
		{
			const std::string id = std::string(Path::GetFileName(fd.FileName));
			if (seen.find(id) != seen.end())
				continue;

			ModEntry entry = loadModMetadata(id, fd.FileName);
			entry.enabled = enabled_map.find(id) != enabled_map.end();
			m_mods.push_back(std::move(entry));
		}
	}
}

void ModSettingsWidget::loadSettings()
{
	m_root_edit->setText(QString::fromStdString(getModsRoot()));
	scanMods();
	rebuildTable();
}

void ModSettingsWidget::saveSettings()
{
	auto* sif = dialog()->getSettingsInterface();
	if (!sif)
		return;

	std::vector<std::string> order;
	std::vector<std::string> enabled;
	order.reserve(m_mods.size());
	enabled.reserve(m_mods.size());

	for (const ModEntry& mod : m_mods)
	{
		order.push_back(mod.id);
		if (mod.enabled)
			enabled.push_back(mod.id);
	}

	sif->SetStringList("Mods", "Order", order);
	sif->SetStringList("Mods", "Enabled", enabled);
	dialog()->saveAndReloadGameSettings();
}

void ModSettingsWidget::rebuildTable()
{
	m_updating = true;
	m_table->setRowCount(static_cast<int>(m_mods.size()));

	for (int row = 0; row < static_cast<int>(m_mods.size()); row++)
	{
		const ModEntry& mod = m_mods[row];

		QTableWidgetItem* enabled_item = new QTableWidgetItem();
		enabled_item->setCheckState(mod.enabled ? Qt::Checked : Qt::Unchecked);
		enabled_item->setData(Qt::UserRole, QString::fromStdString(mod.id));
		m_table->setItem(row, 0, enabled_item);

		m_table->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(mod.name)));
		m_table->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(mod.version)));
		m_table->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(mod.author)));
		m_table->setItem(row, 4, new QTableWidgetItem(QString::fromStdString(mod.path)));
	}

	m_updating = false;
}

void ModSettingsWidget::onBrowseRootClicked()
{
	const QString path = QFileDialog::getExistingDirectory(this, tr("Select Mod Folder"), m_root_edit->text());
	if (path.isEmpty())
		return;

	m_root_edit->setText(QDir::toNativeSeparators(path));
	setModsRoot(path.toStdString());
	loadSettings();
}

void ModSettingsWidget::onRefreshClicked()
{
	loadSettings();
}

void ModSettingsWidget::onAddClicked()
{
	const QString start_dir = m_root_edit->text();
	const QString path = QFileDialog::getExistingDirectory(this, tr("Select Mod Folder"), start_dir);
	if (path.isEmpty())
		return;

	const std::string mod_path = path.toStdString();
	std::string id;
	const std::string root = getModsRoot();

	if (!root.empty() && StringUtil::StartsWithNoCase(mod_path, root))
	{
		id = mod_path.substr(root.size());
		while (!id.empty() && (id.front() == '\\' || id.front() == '/'))
			id.erase(id.begin());
		if (id.empty())
			id = Path::GetFileName(mod_path);
	}
	else
	{
		id = mod_path;
	}

	for (const ModEntry& entry : m_mods)
	{
		if (StringUtil::compareNoCase(entry.id, id))
			return;
	}

	ModEntry entry = loadModMetadata(id, mod_path);
	entry.enabled = true;
	m_mods.push_back(std::move(entry));
	rebuildTable();
	saveSettings();
}

void ModSettingsWidget::onRemoveClicked()
{
	const int row = m_table->currentRow();
	if (row < 0 || row >= static_cast<int>(m_mods.size()))
		return;

	m_mods.erase(m_mods.begin() + row);
	rebuildTable();
	saveSettings();
}

void ModSettingsWidget::onMoveUpClicked()
{
	const int row = m_table->currentRow();
	if (row <= 0 || row >= static_cast<int>(m_mods.size()))
		return;

	std::swap(m_mods[row], m_mods[row - 1]);
	rebuildTable();
	m_table->selectRow(row - 1);
	saveSettings();
}

void ModSettingsWidget::onMoveDownClicked()
{
	const int row = m_table->currentRow();
	if (row < 0 || row >= static_cast<int>(m_mods.size() - 1))
		return;

	std::swap(m_mods[row], m_mods[row + 1]);
	rebuildTable();
	m_table->selectRow(row + 1);
	saveSettings();
}

void ModSettingsWidget::onTableCellChanged(int row, int column)
{
	if (m_updating || column != 0)
		return;

	if (row < 0 || row >= static_cast<int>(m_mods.size()))
		return;

	QTableWidgetItem* item = m_table->item(row, 0);
	if (!item)
		return;

	m_mods[row].enabled = (item->checkState() == Qt::Checked);
	saveSettings();
}
