/*
	Copyright 2014 Ilya Zhuravlev

	This file is part of Acquisition.

	Acquisition is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Acquisition is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Acquisition.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <fstream>
#include <iostream>
#include <vector>
#include <QEvent>
#include <QImageReader>
#include <QInputDialog>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QString>
#include <QStringList>
#include <QTabBar>
#include <QVersionNumber>
#include <QStringListModel>
#include "QsLog.h"

#include "application.h"
#include "buyoutmanager.h"
#include "currencymanager.h"
#include "datastore.h"
#include "filesystem.h"
#include "filters.h"
#include "flowlayout.h"
#include "imagecache.h"
#include "item.h"
#include "itemcategories.h"
#include "itemconstants.h"
#include "itemlocation.h"
#include "itemtooltip.h"
#include "itemsmanager.h"
#include "items_model.h"
#include "logpanel.h"
#include "modsfilter.h"
#include "network_info.h"
#include "ratelimit.h"
#include "ratelimitdialog.h"
#include "ratelimiter.h"
#include "replytimeout.h"
#include "search.h"
#include "selfdestructingreply.h"
#include "shop.h"
#include "updatechecker.h"
#include "util.h"
#include "version_defines.h"
#include "verticalscrollarea.h"

const std::string POE_WEBCDN = "http://webcdn.pathofexile.com"; // Should be updated to https://web.poecdn.com ?

MainWindow::MainWindow(Application& app) :
	app_(app),
	ui(new Ui::MainWindow),
	current_search_(nullptr),
	search_count_(0),
	rate_limit_dialog_(nullptr),
	quitting_(false)
{
	setWindowIcon(QIcon(":/icons/assets/icon.svg"));

	image_cache_ = new ImageCache(Filesystem::UserDir() + "/cache");

	connect(qApp, &QCoreApplication::aboutToQuit, this, [&]() { quitting_ = true; });

	InitializeUi();
	InitializeRateLimitDialog();
	InitializeLogging();
	InitializeSearchForm();

	connect(&delayed_update_current_item_, &QTimer::timeout, this, [&]() {UpdateCurrentItem(); delayed_update_current_item_.stop(); });
	connect(&delayed_search_form_change_, &QTimer::timeout, this, [&]() {OnSearchFormChange(); delayed_search_form_change_.stop(); });
}

MainWindow::~MainWindow() {
	delete ui;
	for (auto& search : searches_) {
		delete(search);
	};
	rate_limit_dialog_->close();
	rate_limit_dialog_->deleteLater();
}

void MainWindow::InitializeRateLimitDialog() {
	rate_limit_dialog_ = new RateLimitDialog(this, &app_.rate_limiter());
	auto* const button = new QPushButton(this);
	button->setFlat(false);
	button->setText("Rate Limit Status");
	connect(button, &QPushButton::clicked, rate_limit_dialog_, &RateLimitDialog::show);
	connect(&app_.rate_limiter(), &RateLimiter::Paused, this,
		[=](int pause) {
			if (pause > 0) {
				button->setText("Rate limited for " + QString::number(pause) + " seconds");
				button->setStyleSheet("font-weight: bold; color: red");
			} else if (pause == 0) {
				button->setText("Rate limiting is OFF");
				button->setStyleSheet("");
			} else {
				button->setText("ERROR: pause is " + QString::number(pause));
				button->setStyleSheet("");
			};
		});
	statusBar()->addPermanentWidget(button);
}

void MainWindow::InitializeLogging() {
	LogPanel* log_panel = new LogPanel(this, ui);
	QsLogging::DestinationPtr log_panel_ptr(log_panel);
	QsLogging::Logger::instance().addDestination(log_panel_ptr);

	// display warnings here so it's more visible
#if defined(_DEBUG)
	QLOG_WARN() << "Maintainer: This is a debug build";
#endif
}

void MainWindow::InitializeUi() {
	ui->setupUi(this);

	status_bar_label_ = new QLabel("Ready");
	statusBar()->addWidget(status_bar_label_);
	ui->itemLayout->setAlignment(Qt::AlignTop);
	ui->itemLayout->setAlignment(ui->minimapLabel, Qt::AlignHCenter);
	ui->itemLayout->setAlignment(ui->nameLabel, Qt::AlignHCenter);
	ui->itemLayout->setAlignment(ui->imageLabel, Qt::AlignHCenter);
	ui->itemLayout->setAlignment(ui->locationLabel, Qt::AlignHCenter);

	tab_bar_ = new QTabBar;
	tab_bar_->installEventFilter(this);
	tab_bar_->setExpanding(false);
	tab_bar_->addTab("+");
	connect(tab_bar_, &QTabBar::currentChanged, this, &MainWindow::OnTabChange);
	ui->mainLayout->insertWidget(0, tab_bar_);

	Util::PopulateBuyoutTypeComboBox(ui->buyoutTypeComboBox);
	Util::PopulateBuyoutCurrencyComboBox(ui->buyoutCurrencyComboBox);

	connect(ui->buyoutCurrencyComboBox, &QComboBox::activated, this, &MainWindow::OnBuyoutChange);
	connect(ui->buyoutTypeComboBox, &QComboBox::activated, this, &MainWindow::OnBuyoutChange);
	connect(ui->buyoutValueLineEdit, &QLineEdit::textEdited, this, &MainWindow::OnBuyoutChange);

	ui->viewComboBox->addItems({ "By Tab", "By Item" });
	connect(ui->viewComboBox, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated), this, [&](int mode) {
		current_search_->SetViewMode(static_cast<Search::ViewMode>(mode));
		if (mode == Search::ByItem) {
			OnExpandAll();
		} else {
			ResizeTreeColumns();
		}
		});

	ui->buyoutTypeComboBox->setEnabled(false);
	ui->buyoutValueLineEdit->setEnabled(false);
	ui->buyoutCurrencyComboBox->setEnabled(false);

	search_form_layout_ = new QVBoxLayout;
	search_form_layout_->setAlignment(Qt::AlignTop);
	search_form_layout_->setContentsMargins(0, 0, 0, 0);

	auto search_form_container = new QWidget;
	search_form_container->setLayout(search_form_layout_);

	auto scroll_area = new VerticalScrollArea;
	scroll_area->setFrameShape(QFrame::NoFrame);
	scroll_area->setWidgetResizable(true);
	scroll_area->setWidget(search_form_container);
	scroll_area->setMinimumWidth(150); // TODO(xyz): remove magic numbers
	scroll_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	ui->scrollArea->setFrameShape(QFrame::NoFrame);
	ui->scrollArea->setWidgetResizable(true);

	ui->horizontalLayout_2->insertWidget(0, scroll_area);
	search_form_container->show();

	ui->horizontalLayout_2->setStretchFactor(0, 2);
	ui->horizontalLayout_2->setStretchFactor(1, 5);
	ui->horizontalLayout_2->setStretchFactor(2, 0);

	ui->treeView->setContextMenuPolicy(Qt::CustomContextMenu);
	ui->treeView->setSelectionMode(QAbstractItemView::ExtendedSelection);
	ui->treeView->setSortingEnabled(true);

	context_menu_.addAction("Refresh Selected", this, &MainWindow::OnRefreshSelected);
	context_menu_.addAction("Check Selected", this, &MainWindow::OnCheckSelected);
	context_menu_.addAction("Uncheck Selected", this, &MainWindow::OnUncheckSelected);
	context_menu_.addSeparator();
	context_menu_.addAction("Check All", this, &MainWindow::OnCheckAll);
	context_menu_.addAction("Uncheck All", this, &MainWindow::OnUncheckAll);
	context_menu_.addSeparator();
	context_menu_.addAction("Expand All", this, &MainWindow::OnExpandAll);
	context_menu_.addAction("Collapse All", this, &MainWindow::OnCollapseAll);

	connect(ui->treeView, &QTreeView::customContextMenuRequested, this, [&](const QPoint& pos) {
		context_menu_.popup(ui->treeView->viewport()->mapToGlobal(pos));
		});

	refresh_button_.setStyleSheet("color: blue; font-weight: bold;");
	refresh_button_.setFlat(true);
	refresh_button_.hide();
	statusBar()->addPermanentWidget(&refresh_button_);
	connect(&refresh_button_, &QPushButton::clicked, this, &MainWindow::OnRefreshAllTabs);

	update_button_.setText("Update available");
	update_button_.setStyleSheet("color: blue; font-weight: bold;");
	update_button_.setFlat(true);
	update_button_.hide();
	statusBar()->addPermanentWidget(&update_button_);
	connect(&update_button_, &QPushButton::clicked, &app_.update_checker(), &UpdateChecker::AskUserToUpdate);

	// resize columns when a tab is expanded/collapsed
	connect(ui->treeView, &QTreeView::collapsed, this, &MainWindow::ResizeTreeColumns);
	connect(ui->treeView, &QTreeView::expanded, this, &MainWindow::ResizeTreeColumns);

	ui->propertiesLabel->setStyleSheet("QLabel { background-color: black; color: #7f7f7f; padding: 10px; font-size: 17px; }");
	ui->propertiesLabel->setFont(QFont("Fontin SmallCaps"));
	ui->itemNameFirstLine->setFont(QFont("Fontin SmallCaps"));
	ui->itemNameSecondLine->setFont(QFont("Fontin SmallCaps"));
	ui->itemNameFirstLine->setAlignment(Qt::AlignCenter);
	ui->itemNameSecondLine->setAlignment(Qt::AlignCenter);

	ui->itemTextTooltip->setStyleSheet("QLabel { background-color: black; color: #7f7f7f; padding: 3px; }");

	ui->itemTooltipWidget->hide();
	ui->itemButtonsWidget->hide();

	// Make sure the right logging level menu item is checked.
	OnSetLogging(QsLogging::Logger::instance().loggingLevel());

	connect(ui->itemInfoTypeTabs, &QTabWidget::currentChanged, this, [=](int idx) {
		auto tabs = ui->itemInfoTypeTabs;
		for (int i = 0; i < tabs->count(); i++)
			if (i != idx)
				tabs->widget(i)->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
		tabs->widget(idx)->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
		tabs->widget(idx)->resize(tabs->widget(idx)->minimumSizeHint());
		tabs->widget(idx)->adjustSize();

		app_.data().SetInt("preferred_tooltip_type", idx);
		});

	// Connect the Tabs menu
	connect(ui->actionRefreshCheckedTabs, &QAction::triggered, this, &MainWindow::OnRefreshCheckedTabs);
	connect(ui->actionRefreshAllTabs, &QAction::triggered, this, &MainWindow::OnRefreshAllTabs);
	connect(ui->actionSetAutomaticTabRefresh, &QAction::triggered, this, &MainWindow::OnSetAutomaticTabRefresh);
	connect(ui->actionSetTabRefreshInterval, &QAction::triggered, this, &MainWindow::OnSetTabRefreshInterval);

	// Connect the Shop menu
	connect(ui->actionSetShopThreads, &QAction::triggered, this, &MainWindow::OnSetShopThreads);
	connect(ui->actionEditShopTemplate, &QAction::triggered, this, &MainWindow::OnEditShopTemplate);
	connect(ui->actionCopyShopToClipboard, &QAction::triggered, this, &MainWindow::OnCopyShopToClipboard);
	connect(ui->actionUpdateShops, &QAction::triggered, this, &MainWindow::OnUpdateShops);
	connect(ui->actionSetAutomaticallyShopUpdate, &QAction::triggered, this, &MainWindow::OnSetAutomaticShopUpdate);
	connect(ui->actionUpdatePOESESSID, &QAction::triggered, this, &MainWindow::OnUpdatePOESESSID);

	// Connect the Currency menu
	connect(ui->actionListCurrency, &QAction::triggered, this, &MainWindow::OnListCurrency);
	connect(ui->actionExportCurrency, &QAction::triggered, this, &MainWindow::OnExportCurrency);

	// Connect the Theme submenu
	connect(ui->actionSetDarkTheme, &QAction::triggered, this, &MainWindow::OnSetDarkTheme);
	connect(ui->actionSetLightTheme, &QAction::triggered, this, &MainWindow::OnSetLightTheme);
	connect(ui->actionSetDefaultTheme, &QAction::triggered, this, &MainWindow::OnSetDefaultTheme);

	// Connect the Logging submenu
	connect(ui->actionLoggingOFF, &QAction::triggered, this, [=]() { OnSetLogging(QsLogging::OffLevel); });
	connect(ui->actionLoggingFATAL, &QAction::triggered, this, [=]() { OnSetLogging(QsLogging::FatalLevel); });
	connect(ui->actionLoggingERROR, &QAction::triggered, this, [=]() { OnSetLogging(QsLogging::ErrorLevel); });
	connect(ui->actionLoggingWARN, &QAction::triggered, this, [=]() { OnSetLogging(QsLogging::WarnLevel); });
	connect(ui->actionLoggingINFO, &QAction::triggered, this, [=]() { OnSetLogging(QsLogging::InfoLevel); });
	connect(ui->actionLoggingDEBUG, &QAction::triggered, this, [=]() { OnSetLogging(QsLogging::DebugLevel); });
	connect(ui->actionLoggingTRACE, &QAction::triggered, this, [=]() { OnSetLogging(QsLogging::TraceLevel); });

	// Connect the Tooltip tab buttons
	connect(ui->uploadTooltipButton, &QPushButton::clicked, this, &MainWindow::OnUploadToImgur);
	connect(ui->pobTooltipButton, &QPushButton::clicked, this, &MainWindow::OnCopyForPOB);
}

void MainWindow::LoadSettings() {

	// Load the appropriate theme.
	const std::string theme = app_.global_data().Get("theme", "default");
	if (theme == "dark") OnSetDarkTheme(true);
	else if (theme == "light") OnSetLightTheme(true);
	else if (theme == "default") OnSetDefaultTheme(true);

	ui->actionSetAutomaticTabRefresh->setChecked(app_.items_manager().auto_update());
	UpdateShopMenu();

	ui->itemInfoTypeTabs->setCurrentIndex(app_.data().GetInt("preferred_tooltip_type"));

	NewSearch();
}

void MainWindow::OnExpandAll() {
	// Only need to expand the top level, which corresponds to buckets,
	// aka stash tabs and characters. Signals are blocked during this
	// operation, otherwise the column resize function connected to
	// the expanded() signal would be called repeatedly.
	setCursor(Qt::WaitCursor);
	ui->treeView->blockSignals(true);
	ui->treeView->expandToDepth(0);
	ui->treeView->blockSignals(false);
	ResizeTreeColumns();
	unsetCursor();
}

void MainWindow::OnCollapseAll() {
	// There is no depth-based collapse method, so manuall looping
	// over rows can be much faster than collapseAll() under some
	// conditions, possibly beecause those funcitons check every
	// element in the tree, which in our case will include all items.
	// 
	// Signals are blocked for the same reason as the expand all case.
	setCursor(Qt::WaitCursor);
	ui->treeView->blockSignals(true);
	const auto& model = *ui->treeView->model();
	const int rowCount = model.rowCount();
	for (int row = 0; row < rowCount; ++row) {
		const QModelIndex idx = model.index(row, 0, QModelIndex());
		ui->treeView->collapse(idx);
	};
	ui->treeView->blockSignals(false);
	ResizeTreeColumns();
	unsetCursor();
}

void MainWindow::OnCheckAll() {
	auto& bo = app_.buyout_manager();
	for (auto const& bucket : current_search_->buckets())
		bo.SetRefreshChecked(bucket->location(), true);
	emit ui->treeView->model()->layoutChanged();
}

void MainWindow::OnUncheckAll() {
	auto& bo = app_.buyout_manager();
	for (auto const& bucket : current_search_->buckets())
		bo.SetRefreshChecked(bucket->location(), false);
	emit ui->treeView->model()->layoutChanged();
}

void MainWindow::OnRefreshSelected() {
	// Get names of tabs to refresh
	std::vector<ItemLocation> locations;
	for (auto const& index : ui->treeView->selectionModel()->selectedRows()) {
		// Fetch tab names per index
		locations.push_back(current_search_->GetTabLocation(index));
	}

	app_.items_manager().Update(TabSelection::Selected, locations);
}

/**
 * @brief MainWindow::CheckSelected
 * @param value test
 */
void MainWindow::CheckSelected(bool value) {
	auto& bo = app_.buyout_manager();

	for (auto const& index : ui->treeView->selectionModel()->selectedRows()) {
		bo.SetRefreshChecked(current_search_->GetTabLocation(index), value);
	}
}

void MainWindow::ResizeTreeColumns() {
	QLOG_DEBUG() << "ResizeTreeColumns";
	for (int i = 0; i < ui->treeView->header()->count(); ++i)
		ui->treeView->resizeColumnToContents(i);
}

void MainWindow::OnBuyoutChange() {
	app_.shop().ExpireShopData();

	Buyout bo;
	bo.type = Buyout::IndexAsBuyoutType(ui->buyoutTypeComboBox->currentIndex());
	bo.currency = Currency::FromIndex(ui->buyoutCurrencyComboBox->currentIndex());
	bo.value = ui->buyoutValueLineEdit->text().replace(',', ".").toDouble();
	bo.last_update = QDateTime::currentDateTime();

	if (bo.IsPriced()) {
		ui->buyoutCurrencyComboBox->setEnabled(true);
		ui->buyoutValueLineEdit->setEnabled(true);
	} else {
		ui->buyoutCurrencyComboBox->setEnabled(false);
		ui->buyoutValueLineEdit->setEnabled(false);
	}

	if (!bo.IsValid())
		return;

	// Don't assign a zero buyout if nothing is entered in the value textbox
	if (ui->buyoutValueLineEdit->text().isEmpty() && bo.IsPriced())
		return;

	BuyoutManager& bo_manager = app_.buyout_manager();
	for (auto const& index : ui->treeView->selectionModel()->selectedRows()) {
		auto const& tab = current_search_->GetTabLocation(index).GetUniqueHash();

		// Don't allow users to manually update locked tabs (game priced)
		if (bo_manager.GetTab(tab).IsGameSet())
			continue;
		if (!index.parent().isValid()) {
			bo_manager.SetTab(tab, bo);
		} else {
			auto& item = current_search_->bucket(index.parent().row())->item(index.row());
			// Don't allow users to manually update locked items (game priced per item in note section)
			if (bo_manager.Get(*item).IsGameSet())
				continue;
			bo_manager.Set(*item, bo);
		}
	}
	app_.items_manager().PropagateTabBuyouts();
	ResizeTreeColumns();
}

void MainWindow::OnStatusUpdate(ProgramState state, const QString& message) {
	QString status;
	switch (state) {
	case ProgramState::Initializing: status = "Initializing"; break;
	case ProgramState::Ready: status = "Ready"; break;
	case ProgramState::Busy: status = "Busy"; break;
	case ProgramState::Waiting: status = "Waiting"; break;
	case ProgramState::Unknown: status = "Unknown State"; break;
		/*
		case ProgramState::CharactersReceived:
			if (status.total == 0) {
				refresh_button_.setText("No characters detected yet in this league, click to refresh");
				refresh_button_.show();
			} else {
				refresh_button_.hide();
			}
			break;
		case ProgramState::ItemsReceive:
		case ProgramState::ItemsPaused:
			title = QString("Receiving stash data, %1/%2").arg(status.progress).arg(status.total);
			if (status.state == ProgramState::ItemsPaused)
				title += " (throttled, sleeping 60 seconds)";
			break;
		case ProgramState::ItemsCompleted:
			title = QString("Received %1 tabs").arg(status.total);
			QLOG_INFO() << title;
			break;
		case ProgramState::ShopSubmitting:
			title = QString("Sending your shops to the forum, %1/%2").arg(status.progress).arg(status.total);
			break;
		case ProgramState::ShopCompleted:
			title = QString("Shop threads updated");
			QLOG_INFO() << title;
			break;
		case ProgramState::UpdateCancelled:
			title = QString("Shop updated cancelled due to tab movement or rename mid-update");
			break;
		case ProgramState::ItemsRetrieved:
			title = QString("Parsing item mods in tabs, %1/%2").arg(status.progress).arg(status.total);
			break;
		default:
			title = "Unknown";
			*/
	};
	if (!message.isEmpty()) {
		status += ": " + message;
	};
	status_bar_label_->setText(status);
	status_bar_label_->update();
}

bool MainWindow::eventFilter(QObject* o, QEvent* e) {
	if (o == tab_bar_ && e->type() == QEvent::MouseButtonPress) {
		QMouseEvent* mouse_event = static_cast<QMouseEvent*>(e);
		int index = tab_bar_->tabAt(mouse_event->pos());
		if (mouse_event->button() == Qt::MiddleButton) {
			// remove tab and Search if it's not "+"
			if (index >= 0 && index < tab_bar_->count() - 1) {
				tab_bar_->removeTab(index);
				auto search = searches_[index];
				if (previous_search_ == search)
					previous_search_ = nullptr;
				if (current_search_ == search)
					current_search_ = nullptr;
				delete searches_[index];
				searches_.erase(searches_.begin() + index);
				if (static_cast<size_t>(tab_bar_->currentIndex()) == searches_.size())
					tab_bar_->setCurrentIndex(static_cast<int>(searches_.size()) - 1);
				OnTabChange(tab_bar_->currentIndex());
				// that's because after removeTab text will be set to previous search's caption
				// which is because my way of dealing with "+" tab is hacky and should be replaced by something sane
				tab_bar_->setTabText(tab_bar_->count() - 1, "+");
			}
			return true;
		} else if (mouse_event->button() == Qt::RightButton) {
			rightClickedTabIndex = index;

			if (rightClickedTabIndex >= 0 && rightClickedTabIndex < tab_bar_->count() - 1) {
				class TabRightClickMenu :public QMenu {};
				TabRightClickMenu rcMenu;

				rcMenu.addAction("Rename Tab", this, &MainWindow::OnRenameTabClicked);
				rcMenu.exec(QCursor::pos());
			}

			rightClickedTabIndex = -1;
		}
	}
	return QMainWindow::eventFilter(o, e);
}

void MainWindow::OnRenameTabClicked() {
	bool ok;
	QString name = QInputDialog::getText(this, "Rename Tab",
		"Rename Tab here",
		QLineEdit::Normal, "", &ok);

	if (ok && !name.isEmpty()) {
		searches_[rightClickedTabIndex]->RenameCaption(name.toStdString());
		tab_bar_->setTabText(rightClickedTabIndex, searches_[rightClickedTabIndex]->GetCaption());
	}
}

void MainWindow::OnImageFetched(QNetworkReply* reply) {
	std::string url = reply->url().toString().toStdString();
	if (reply->error()) {
		QLOG_WARN() << "Failed to download item image," << url.c_str();
		return;
	}
	QImageReader image_reader(reply);
	QImage image = image_reader.read();

	image_cache_->Set(url, image);

	if (current_item_ && (url == current_item_->icon() || url == POE_WEBCDN + current_item_->icon()))
		ui->imageLabel->setPixmap(GenerateItemIcon(*current_item_, image));
}

void MainWindow::SetCurrentSearch(Search* search) {
	previous_search_ = current_search_;
	current_search_ = search;
}

void MainWindow::OnSearchFormChange() {
	current_search_->SetRefreshReason(RefreshReason::SearchFormChanged);
	ModelViewRefresh();
}

void MainWindow::ModelViewRefresh() {
	app_.buyout_manager().Save();

	// Save view properties if no search fields are populated
	// AND we're viewing in Tab mode
	if (previous_search_ && !previous_search_->IsAnyFilterActive()
		&& previous_search_->GetViewMode() == Search::ByTab)
		previous_search_->SaveViewProperties();

	previous_search_ = current_search_;

	current_search_->Activate(app_.items_manager().items());

	// This updates the item information when current item changes.
	connect(ui->treeView->selectionModel(), &QItemSelectionModel::currentChanged, this, &MainWindow::OnCurrentItemChanged);

	// This updates the item information when a search or sort order changes.
	connect(ui->treeView->model(), &QAbstractItemModel::layoutChanged, this, &MainWindow::OnLayoutChanged);

	ui->viewComboBox->setCurrentIndex(static_cast<int>(current_search_->GetViewMode()));

	QLOG_DEBUG() << "Skipping tree view reset";
	//ui->treeView->reset();
	if (current_search_->IsAnyFilterActive() || current_search_->GetViewMode() == Search::ByItem) {
		// Policy is to expand all tabs when any search fields are populated
		// Also expand by default if we're in Item view mode
		OnExpandAll();
	} else {
		// Restore view properties if no search fields are populated AND current mode is tab mode
		current_search_->RestoreViewProperties();
		ResizeTreeColumns();
	}

	tab_bar_->setTabText(tab_bar_->currentIndex(), current_search_->GetCaption());
}

void MainWindow::OnCurrentItemChanged(const QModelIndex& current, const QModelIndex& previous) {
	Q_UNUSED(previous);
	app_.buyout_manager().Save();
	if (!current.parent().isValid()) {
		// clicked on a bucket
		current_item_ = nullptr;
		current_bucket_ = *current_search_->bucket(current.row());
		UpdateCurrentBucket();
	} else {
		// clicked on an item
		current_item_ = current_search_->bucket(current.parent().row())->item(current.row());
		delayed_update_current_item_.start(100);
	};
	UpdateCurrentBuyout();
}

void MainWindow::OnLayoutChanged() {
	// Do nothing is nothing is selected.
	if (current_item_ == nullptr)
		return;

	// Look for the new index of the currently selected item.
	const QModelIndex idx = current_search_->index(current_item_);

	if (!idx.isValid()) {
		// The previously selected item is no longer in search results.
		current_item_ = nullptr;
		ClearCurrentItem();
		ui->treeView->selectionModel()->clearSelection();
	} else {
		// Reselect the item in the updated layout.
		ui->treeView->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);;
	};
}

void MainWindow::OnDelayedSearchFormChange() {
	// wait 350ms after search form change before applying
	// This is so we don't force update after every keystroke etc...
	delayed_search_form_change_.start(350);
}

void MainWindow::OnTabChange(int index) {
	// "+" clicked
	if (static_cast<size_t>(index) == searches_.size()) {
		NewSearch();
	} else {
		SetCurrentSearch(searches_[index]);
		current_search_->SetRefreshReason(RefreshReason::TabChanged);
		current_search_->ToForm();
		ModelViewRefresh();
	}
}

void MainWindow::AddSearchGroup(QLayout* layout, const std::string& name = "") {
	if (!name.empty()) {
		auto label = new QLabel(("<h3>" + name + "</h3>").c_str());
		search_form_layout_->addWidget(label);
	}
	layout->setContentsMargins(0, 0, 0, 0);
	auto layout_container = new QWidget;
	layout_container->setLayout(layout);
	search_form_layout_->addWidget(layout_container);
}

void MainWindow::InitializeSearchForm() {
	category_string_model_ = new QStringListModel;
	rarity_search_model_ = new QStringListModel;
	rarity_search_model_->setStringList(RaritySearchFilter::RARITY_LIST);
	auto name_search = std::make_unique<NameSearchFilter>(search_form_layout_);
	auto category_search = std::make_unique<CategorySearchFilter>(search_form_layout_, category_string_model_);
	auto rarity_search = std::make_unique<RaritySearchFilter>(search_form_layout_, rarity_search_model_);
	auto offense_layout = new FlowLayout;
	auto defense_layout = new FlowLayout;
	auto sockets_layout = new FlowLayout;
	auto requirements_layout = new FlowLayout;
	auto misc_layout = new FlowLayout;
	auto misc_flags_layout = new FlowLayout;
	auto misc_flags2_layout = new FlowLayout;
	auto mods_layout = new QHBoxLayout;

	AddSearchGroup(offense_layout, "Offense");
	AddSearchGroup(defense_layout, "Defense");
	AddSearchGroup(sockets_layout, "Sockets");
	AddSearchGroup(requirements_layout, "Requirements");
	AddSearchGroup(misc_layout, "Misc");
	AddSearchGroup(misc_flags_layout);
	AddSearchGroup(misc_flags2_layout);
	AddSearchGroup(mods_layout, "Mods");

	using move_only = std::unique_ptr<Filter>;
	move_only init[] = {
		std::move(name_search),
		std::move(category_search),
		std::move(rarity_search),
		// Offense
		// new DamageFilter(offense_layout, "Damage"),
		std::make_unique<SimplePropertyFilter>(offense_layout, "Critical Strike Chance", "Crit."),
		std::make_unique<ItemMethodFilter>(offense_layout, [](Item* item) { return item->DPS(); }, "DPS"),
		std::make_unique<ItemMethodFilter>(offense_layout, [](Item* item) { return item->pDPS(); }, "pDPS"),
		std::make_unique<ItemMethodFilter>(offense_layout, [](Item* item) { return item->eDPS(); }, "eDPS"),
		std::make_unique<ItemMethodFilter>(offense_layout, [](Item* item) { return item->cDPS(); }, "cDPS"),
		std::make_unique<SimplePropertyFilter>(offense_layout, "Attacks per Second", "APS"),
		// Defense
		std::make_unique<SimplePropertyFilter>(defense_layout, "Armour"),
		std::make_unique<SimplePropertyFilter>(defense_layout, "Evasion Rating", "Evasion"),
		std::make_unique<SimplePropertyFilter>(defense_layout, "Energy Shield", "Shield"),
		std::make_unique<SimplePropertyFilter>(defense_layout, "Chance to Block", "Block"),
		// Sockets
		std::make_unique<SocketsFilter>(sockets_layout, "Sockets"),
		std::make_unique<LinksFilter>(sockets_layout, "Links"),
		std::make_unique<SocketsColorsFilter>(sockets_layout),
		std::make_unique<LinksColorsFilter>(sockets_layout),
		// Requirements
		std::make_unique<RequiredStatFilter>(requirements_layout, "Level", "R. Level"),
		std::make_unique<RequiredStatFilter>(requirements_layout, "Str", "R. Str"),
		std::make_unique<RequiredStatFilter>(requirements_layout, "Dex", "R. Dex"),
		std::make_unique<RequiredStatFilter>(requirements_layout, "Int", "R. Int"),
		// Misc
		std::make_unique<DefaultPropertyFilter>(misc_layout, "Quality", 0),
		std::make_unique<SimplePropertyFilter>(misc_layout, "Level"),
		std::make_unique<SimplePropertyFilter>(misc_layout, "Map Tier"),
		std::make_unique<ItemlevelFilter>(misc_layout, "ilvl"),
		std::make_unique<AltartFilter>(misc_flags_layout, "", "Alt. art"),
		std::make_unique<PricedFilter>(misc_flags_layout, "", "Priced", app_.buyout_manager()),
		std::make_unique<UnidentifiedFilter>(misc_flags2_layout, "", "Unidentified"),
		std::make_unique<InfluencedFilter>(misc_flags2_layout, "", "Influenced"),
		std::make_unique<CraftedFilter>(misc_flags2_layout, "", "Master-crafted"),
		std::make_unique<EnchantedFilter>(misc_flags2_layout, "", "Enchanted"),
		std::make_unique<CorruptedFilter>(misc_flags2_layout, "", "Corrupted"),
		std::make_unique<ModsFilter>(mods_layout)
	};
	filters_ = std::vector<move_only>(std::make_move_iterator(std::begin(init)), std::make_move_iterator(std::end(init)));
}

void MainWindow::NewSearch() {
	SetCurrentSearch(new Search(app_.buyout_manager(), QString("Search %1").arg(++search_count_).toStdString(), filters_, ui->treeView));
	current_search_->SetRefreshReason(RefreshReason::TabCreated);

	tab_bar_->setTabText(tab_bar_->count() - 1, current_search_->GetCaption());

	tab_bar_->addTab("+");
	// this can't be done in ctor because it'll call OnSearchFormChange slot
	// and remove all previous search data
	current_search_->ResetForm();
	searches_.push_back(current_search_);
	ModelViewRefresh();
}

void MainWindow::ClearCurrentItem() {
	ui->imageLabel->hide();
	ui->minimapLabel->hide();
	ui->locationLabel->hide();
	ui->itemTooltipWidget->hide();
	ui->itemButtonsWidget->hide();

	ui->nameLabel->setText("Select an item");
	ui->nameLabel->show();

	ui->pobTooltipButton->setEnabled(false);
}

void MainWindow::UpdateCurrentBucket() {
	ui->imageLabel->hide();
	ui->minimapLabel->hide();
	ui->locationLabel->hide();
	ui->itemTooltipWidget->hide();
	ui->itemButtonsWidget->hide();

	ui->nameLabel->setText(current_bucket_.location().GetHeader().c_str());
	ui->nameLabel->show();

	ui->pobTooltipButton->setEnabled(false);
}

void MainWindow::UpdateCurrentItem() {
	if (current_item_ == nullptr) {
		ClearCurrentItem();
		return;
	};

	ui->imageLabel->show();
	ui->minimapLabel->show();
	ui->locationLabel->show();
	ui->itemTooltipWidget->show();
	ui->itemButtonsWidget->show();
	ui->nameLabel->hide();

	ui->imageLabel->setText("Loading...");
	ui->imageLabel->setStyleSheet("QLabel { background-color : rgb(12, 12, 43); color: white }");
	ui->imageLabel->setFixedSize(QSize(current_item_->w(), current_item_->h()) * PIXELS_PER_SLOT);

	// Everything except item image now lives in itemtooltip.cpp
	// in future should move everything tooltip-related there
	UpdateItemTooltip(*current_item_, ui);

	ui->pobTooltipButton->setEnabled(current_item_->Wearable());

	std::string icon = current_item_->icon();
	if (icon.size() && icon[0] == '/')
		icon = POE_WEBCDN + icon;
	if (!image_cache_->Exists(icon)) {
		QNetworkRequest request = QNetworkRequest(QUrl(icon.c_str()));
		request.setHeader(QNetworkRequest::KnownHeaders::UserAgentHeader, USER_AGENT);
		QNetworkReply* reply = app_.network_manager().get(request);
		connect(reply, &QNetworkReply::finished, this, [=]() { OnImageFetched(reply); });
	} else
		ui->imageLabel->setPixmap(GenerateItemIcon(*current_item_, image_cache_->Get(icon)));

	ui->locationLabel->setText(current_item_->location().GetHeader().c_str());
}

void MainWindow::UpdateBuyoutWidgets(const Buyout& bo) {
	ui->buyoutTypeComboBox->setCurrentIndex(bo.type);
	ui->buyoutTypeComboBox->setEnabled(!bo.IsGameSet());
	ui->buyoutCurrencyComboBox->setEnabled(false);
	ui->buyoutValueLineEdit->setEnabled(false);

	if (bo.IsPriced()) {
		ui->buyoutCurrencyComboBox->setCurrentIndex(bo.currency.type);
		ui->buyoutValueLineEdit->setText(QString::number(bo.value));
		if (!bo.IsGameSet()) {
			ui->buyoutCurrencyComboBox->setEnabled(true);
			ui->buyoutValueLineEdit->setEnabled(true);
		}
	} else {
		ui->buyoutValueLineEdit->setText("");
	}
}

void MainWindow::UpdateCurrentBuyout() {
	if (current_item_) {
		UpdateBuyoutWidgets(app_.buyout_manager().Get(*current_item_));
	} else {
		std::string tab = current_bucket_.location().GetUniqueHash();
		UpdateBuyoutWidgets(app_.buyout_manager().GetTab(tab));
	}
}

void MainWindow::OnItemsRefreshed() {
	int tab = 0;
	for (auto search : searches_) {
		search->SetRefreshReason(RefreshReason::ItemsChanged);
		// Don't update current search - it will be updated in OnSearchFormChange
		if (search != current_search_) {
			search->FilterItems(app_.items_manager().items());
			tab_bar_->setTabText(tab, search->GetCaption());
		}
		tab++;
	}
	QStringList categories = GetItemCategories();
	category_string_model_->setStringList(categories);
	// Must re-populate category form after model re-init which clears selection
	current_search_->ToForm();

	ModelViewRefresh();
}

void MainWindow::OnSetShopThreads() {
	bool ok;
	QString thread = QInputDialog::getText(this, "Shop thread",
		"Enter thread number. You can enter multiple shops by separating them with a comma. More than one shop may be needed if you have a lot of items.",
		QLineEdit::Normal, Util::StringJoin(app_.shop().threads(), ",").c_str(), &ok);
	if (ok && !thread.isEmpty())
		app_.shop().SetThread(Util::StringSplit(thread.remove(QRegularExpression("\\s+")).toStdString(), ','));
	UpdateShopMenu();
}

void MainWindow::OnUpdatePOESESSID() {
	QLOG_ERROR() << "Shop -> Update POESESSID is not implemented yet.";
}

void MainWindow::UpdateShopMenu() {
	std::string title = "Forum shop thread...";
	if (!app_.shop().threads().empty())
		title += " [" + Util::StringJoin(app_.shop().threads(), ",") + "]";
	ui->actionSetShopThreads->setText(title.c_str());
	ui->actionSetAutomaticallyShopUpdate->setChecked(app_.shop().auto_update());
}

void MainWindow::OnUpdateAvailable() {
	update_button_.show();
}

void MainWindow::OnCopyShopToClipboard() {
	app_.shop().CopyToClipboard();
}

void MainWindow::OnSetTabRefreshInterval() {
	int interval = QInputDialog::getText(this, "Auto refresh items", "Refresh items every X minutes",
		QLineEdit::Normal, QString::number(app_.items_manager().auto_update_interval())).toInt();
	if (interval > 0)
		app_.items_manager().SetAutoUpdateInterval(interval);
}

void MainWindow::OnRefreshAllTabs() {
	app_.items_manager().Update(TabSelection::All);
}

void MainWindow::OnRefreshCheckedTabs() {
	app_.items_manager().Update(TabSelection::Checked);
}

void MainWindow::OnSetAutomaticTabRefresh() {
	app_.items_manager().SetAutoUpdate(ui->actionSetAutomaticTabRefresh->isChecked());
}

void MainWindow::OnUpdateShops() {
	app_.shop().SubmitShopToForum(true);
}

void MainWindow::OnEditShopTemplate() {
	bool ok;
	QString text = QInputDialog::getMultiLineText(this, "Shop template", "Enter shop template. [items] will be replaced with the list of items you marked for sale.",
		app_.shop().shop_template().c_str(), &ok);
	if (ok && !text.isEmpty())
		app_.shop().SetShopTemplate(text.toStdString());
}

void MainWindow::OnSetAutomaticShopUpdate() {
	app_.shop().SetAutoUpdate(ui->actionSetAutomaticallyShopUpdate->isChecked());
}

void MainWindow::OnListCurrency() {
	app_.currency_manager().DisplayCurrency();
}

void MainWindow::OnSetDarkTheme(bool toggle) {
	// Credits: https://github.com/ColinDuquesnoy/QDarkStyleSheet
	if (toggle) {
		QFile f(":qdarkstyle/dark/darkstyle.qss");
		if (!f.exists()) {
			printf("Unable to set stylesheet, file not found\n");
		} else {
			f.open(QFile::ReadOnly | QFile::Text);
			QTextStream ts(&f);
			qApp->setStyleSheet(ts.readAll());

			QPalette pal = QApplication::palette();
			pal.setColor(QPalette::WindowText, Qt::white);
			QApplication::setPalette(pal);
		}
		app_.global_data().Set("theme", "dark");
		ui->actionSetLightTheme->setChecked(false);
		ui->actionSetDefaultTheme->setChecked(false);
	}
	ui->actionSetDarkTheme->setChecked(toggle);
}

void MainWindow::OnSetLightTheme(bool toggle) {
	if (toggle) {
		QFile f(":qdarkstyle/light/lightstyle.qss");
		if (!f.exists()) {
			printf("Unable to set stylesheet, file not found\n");
		} else {
			f.open(QFile::ReadOnly | QFile::Text);
			QTextStream ts(&f);
			qApp->setStyleSheet(ts.readAll());

			QPalette pal = QApplication::palette();
			pal.setColor(QPalette::WindowText, Qt::black);
			QApplication::setPalette(pal);
		}
		app_.global_data().Set("theme", "light");
		ui->actionSetDarkTheme->setChecked(false);
		ui->actionSetDefaultTheme->setChecked(false);
	}
	ui->actionSetLightTheme->setChecked(toggle);
}

void MainWindow::OnSetDefaultTheme(bool toggle) {
	if (toggle) {
		qApp->setStyleSheet("");
		QPalette pal = QApplication::palette();
		pal.setColor(QPalette::WindowText, Qt::black);
		QApplication::setPalette(pal);
		app_.global_data().Set("theme", "default");
		ui->actionSetDarkTheme->setChecked(false);
		ui->actionSetLightTheme->setChecked(false);
	}
	ui->actionSetDefaultTheme->setChecked(toggle);
}

void MainWindow::OnSetLogging(QsLogging::Level level) {
	QsLogging::Logger::instance().setLoggingLevel(level);
	ui->actionLoggingOFF->setChecked(level == QsLogging::OffLevel);
	ui->actionLoggingFATAL->setChecked(level == QsLogging::FatalLevel);
	ui->actionLoggingERROR->setChecked(level == QsLogging::ErrorLevel);
	ui->actionLoggingWARN->setChecked(level == QsLogging::WarnLevel);
	ui->actionLoggingINFO->setChecked(level == QsLogging::InfoLevel);
	ui->actionLoggingDEBUG->setChecked(level == QsLogging::DebugLevel);
	ui->actionLoggingTRACE->setChecked(level == QsLogging::TraceLevel);
	QString new_level;
	switch (level) {
	case QsLogging::OffLevel: new_level = "OFF"; break;
	case QsLogging::FatalLevel: new_level = "FATAL"; break;
	case QsLogging::ErrorLevel: new_level = "ERROR"; break;
	case QsLogging::WarnLevel: new_level = "WARN"; break;
	case QsLogging::InfoLevel: new_level = "INFO"; break;
	case QsLogging::DebugLevel: new_level = "DEBUG"; break;
	case QsLogging::TraceLevel: new_level = "TRACE"; break;
	}
	QLOG_INFO() << "Logging level set to" << new_level;
}

void MainWindow::OnExportCurrency() {
	app_.currency_manager().ExportCurrency();
}

void MainWindow::closeEvent(QCloseEvent* event) {

	if (quitting_) {
		event->accept();
		return;
	};

	QMessageBox msgbox(this);
	msgbox.setWindowTitle("Acquisition");
	msgbox.setText(tr("Are you sure you want to quit?"));
	msgbox.setStandardButtons(QMessageBox::No | QMessageBox::Yes);
	msgbox.setDefaultButton(QMessageBox::Yes);
	
	const auto button = msgbox.exec();
	if (button == QMessageBox::Yes) {
		event->accept();
	} else {
		event->ignore();
	};
}

void MainWindow::OnUploadToImgur() {
	ui->uploadTooltipButton->setDisabled(true);
	ui->uploadTooltipButton->setText("Uploading...");

	QPixmap pixmap(ui->itemTooltipWidget->size());
	ui->itemTooltipWidget->render(&pixmap);

	QByteArray bytes;
	QBuffer buffer(&bytes);
	buffer.open(QIODevice::WriteOnly);
	pixmap.save(&buffer, "PNG"); // writes pixmap into bytes in PNG format

	QNetworkRequest request(QUrl("https://api.imgur.com/3/upload/"));
	request.setRawHeader("Authorization", "Client-ID d6d2d8a0437a90f");
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
	request.setHeader(QNetworkRequest::KnownHeaders::UserAgentHeader, USER_AGENT);
	request.setTransferTimeout(kImgurUploadTimeout);
	QByteArray data = "image=" + QUrl::toPercentEncoding(bytes.toBase64());
	QNetworkReply* reply = app_.network_manager().post(request, data);
	connect(reply, &QNetworkReply::finished, this, &MainWindow::OnUploadFinished);
}

void MainWindow::OnCopyForPOB() {
	if (current_item_ == nullptr) {
		return;
	}
	// if category isn't wearable, including flasks, don't do anything
	if (!current_item_->Wearable()) {
		QLOG_WARN() << current_item_->PrettyName().c_str() << ", category:" << current_item_->category().c_str() << ", should not have been exportable.";
		return;
	}

	QApplication::clipboard()->setText(QString::fromStdString(current_item_->POBformat()));
	QLOG_INFO() << current_item_->PrettyName().c_str() << "was copied to your clipboard in Path of Building's \"Create custom\" format.";
}

void MainWindow::OnUploadFinished() {
	ui->uploadTooltipButton->setDisabled(false);
	ui->uploadTooltipButton->setText("Upload to imgur");

	SelfDestructingReply reply(qobject_cast<QNetworkReply*>(QObject::sender()));
	QByteArray bytes = reply->readAll();

	rapidjson::Document doc;
	doc.Parse(bytes.constData());

	if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("status") || !doc["status"].IsNumber()) {
		QLOG_ERROR() << "Imgur API returned invalid data (or timed out): " << bytes;
		reply->deleteLater();
		return;
	}
	if (doc["status"].GetInt() != 200) {
		QLOG_ERROR() << "Imgur API returned status!=200: " << bytes;
		reply->deleteLater();
		return;
	}
	if (!doc.HasMember("data") || !doc["data"].HasMember("link") || !doc["data"]["link"].IsString()) {
		QLOG_ERROR() << "Imgur API returned malformed reply: " << bytes;
		reply->deleteLater();
		return;
	}
	std::string url = doc["data"]["link"].GetString();
	QApplication::clipboard()->setText(url.c_str());
	QLOG_INFO() << "Image successfully uploaded, the URL is" << url.c_str() << "It also was copied to your clipboard.";

	reply->deleteLater();
}
