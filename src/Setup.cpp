#include "Setup.h"
#include "common/Args.hpp"
#include "common/Channel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Updates.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/splits/Split.hpp"
#include "common/network/NetworkManager.hpp"
#include "providers/NetworkConfigurationProvider.hpp"
#include "providers/IvrApi.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "util/FilesystemHelpers.hpp"
#include "common/Env.hpp"
#include "Application.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QAction>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QPluginLoader>
#include <QPointer>
#include <QResizeEvent>
#include <QSslSocket>
#include <QStringView>
#include <QWidget>
#include <QDockWidget>
#include <QFrame>
#include <QObject>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QUuid>
#include <QVBoxLayout>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class ChatWidget : public QFrame {
public:
	ChatWidget(QString id, QWidget *parent)
		: QFrame(parent),
		  id_(std::move(id)),
		  split_(new chatterino::Split(this))
	{
		this->split_->setChannel(chatterino::Channel::getEmpty());
		this->split_->setSizePolicy({QSizePolicy::Expanding, QSizePolicy::Expanding});
		std::ignore = this->split_->actionRequested.connect([this](chatterino::Split::Action action) {
			if (action == chatterino::Split::Action::Delete) {
				obs_frontend_remove_dock(this->id_.toStdString().c_str());
			}
		});
		std::ignore = this->split_->channelChanged.connect([this] {
			auto *parent = qobject_cast<QDockWidget *>(this->parentWidget());
			if (parent) {
				parent->setWindowTitle(this->split_->getChannel()->getDisplayName());
			}
		});
	}

	chatterino::Split *split() { return this->split_; }
	QStringView id() const { return this->id_; }

protected:
	void resizeEvent(QResizeEvent *event) override { this->split_->setGeometry({QPoint{}, this->size()}); }

private:
	QString id_;
	chatterino::Split *split_;
};

chatterino::Args makeArgs()
{
	chatterino::Args args;
	args.dontSaveSettings = true;
	args.dontLoadMainWindow = true;
	args.isFramelessEmbed = true;
	return args;
}

QString getAppDir()
{
	auto appdata = chatterino::qStringToStdPath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
			       .parent_path();
	return chatterino::stdPathToQString(appdata / "Chatterino2");
}

class PluginState {
public:
	PluginState()
		: paths(getAppDir()),
		  args(makeArgs()),
		  settings(this->args, this->paths.settingsDirectory),
		  updates(this->paths, this->settings),
		  app(this->settings, this->paths, this->args, this->updates)
	{
	}

public:
	void load(obs_data_t *rootData)
	{
		const char *str = obs_data_get_string(rootData, "chatterino-splits");
		if (!str) {
			return;
		}
		// FIXME: use QJsonValue once OBS uses Qt 6.9+
		this->deserialize(QJsonDocument::fromJson(str).object());
	}

	void save(obs_data_t *rootData)
	{
		auto obj = this->serialize();
		QJsonDocument doc(obj);
		auto ba = doc.toJson(QJsonDocument::Compact);
		ba.append('\0'); // I love C strings�
		obs_data_set_string(rootData, "chatterino-splits", ba.constData());
	}

	void deserialize(const QJsonObject &rootObj)
	{
		for (auto it = rootObj.constBegin(); it != rootObj.constEnd(); it++) {
			chatterino::SplitDescriptor descriptor;
			const QJsonObject splitObj = it.value().toObject();
			chatterino::SplitDescriptor::loadFromJSON(descriptor, splitObj, splitObj["data"].toObject());
			auto *chat = this->addChatByID(it.key(), false);
			if (!chat) {
				continue;
			}
			chat->split()->setChannel(chatterino::WindowManager::decodeChannel(descriptor));
			chat->split()->setModerationMode(descriptor.moderationMode_);
			chat->split()->setFilters(descriptor.filters_);
			chat->split()->setCheckSpellingOverride(descriptor.spellCheckOverride);
		}
	}

	QJsonObject serialize() const
	{
		QJsonObject obj;
		for (const auto &[key, val] : this->widgets) {
			if (!val) {
				continue;
			}
			auto *split = val->split();
			QJsonObject splitObj;
			chatterino::WindowManager::encodeSplit(split, splitObj);
			obj[key] = std::move(splitObj);
		}
		return obj;
	}

	void addChat()
	{
		auto uuid = QUuid::createUuid();
		auto id = "chatterino-obs:" + uuid.toString();
		this->addChatByID(id, true);
	}

public:
	chatterino::Paths paths;
	chatterino::Args args;
	chatterino::Settings settings;
	chatterino::Updates updates;
	chatterino::Application app;

private:
	ChatWidget *addChatByID(const QString &id, bool forceVisible)
	{
		auto *chat = new ChatWidget(id, static_cast<QMainWindow *>(obs_frontend_get_main_window()));
		if (!obs_frontend_add_dock_by_id(id.toStdString().c_str(), "Chatterino", chat)) {
			chat->deleteLater();
			return nullptr;
		}

		if (forceVisible) {
			QMetaObject::invokeMethod(
				chat, [chat] { chat->parentWidget()->setVisible(true); }, Qt::QueuedConnection);
		}
		this->widgets.emplace_back(id, chat);
		return chat;
	}

	std::vector<std::pair<QString, QPointer<ChatWidget>>> widgets;
};

#ifdef Q_OS_WIN
QString obsModuleDataPath(const char *name)
{
	char *res = obs_module_file(name);
	if (!res) {
		return {};
	}
	auto str = QString::fromUtf8(res);
	bfree(res);
	return str;
}

void tryLoadTLS(const char *path)
{
	auto qPath = obsModuleDataPath(path);
	if (qPath.isEmpty()) {
		obs_log(LOG_INFO, "%s not found in data, skipping", path);
		return;
	}
	QPluginLoader loader(qPath);
	if (loader.isLoaded()) {
		obs_log(LOG_INFO, "%s is already loaded", path);
		return;
	}
	if (!loader.load()) {
		obs_log(LOG_INFO, "%s failed to load: %s", path, loader.errorString().toStdString().c_str());
		return;
	}
	if (!loader.instance()) {
		obs_log(LOG_INFO, "%s no instance.", path);
		return;
	}
	obs_log(LOG_INFO, "%s loaded (instance: %s)", path, loader.instance()->metaObject()->className());
}

void ensureTLS()
{
	if (!QSslSocket::availableBackends().empty()) {
		return;
	}
#ifdef NDEBUG
	tryLoadTLS("qschannelbackend.dll");
	tryLoadTLS("qcertonlybackend.dll");
	tryLoadTLS("qopensslbackend.dll");
#else
	tryLoadTLS("qschannelbackendd.dll");
	tryLoadTLS("qcertonlybackendd.dll");
	tryLoadTLS("qopensslbackendd.dll");
#endif

	std::string backends = QSslSocket::availableBackends().join(", ").toStdString();
	obs_log(LOG_INFO, "TLS backends: %s", backends.c_str());
}
#endif

std::unique_ptr<PluginState> GLOBAL_STATE;
} // namespace

namespace chatterino::obs {

}

namespace {
extern "C" void chatterino_obs_init()
{
#ifdef Q_OS_WIN
	ensureTLS();
#endif

	chatterino::initResources();
	chatterino::NetworkConfigurationProvider::applyFromEnv(chatterino::Env::get());
	chatterino::IvrApi::initialize();
	chatterino::Helix::initialize();
	GLOBAL_STATE.reset(new PluginState);

	chatterino::NetworkManager::init();
	GLOBAL_STATE->app.initialize(GLOBAL_STATE->settings, GLOBAL_STATE->paths);
	GLOBAL_STATE->app.getTwitch()->connect();
	obs_log(LOG_INFO, "SETTINGS: %s", GLOBAL_STATE->paths.settingsDirectory.toStdString().c_str());

	GLOBAL_STATE->settings.enableBTTVChannelEmotes.connect(
		[] {
			dynamic_cast<chatterino::TwitchIrcServer *>(GLOBAL_STATE->app.getTwitch())
				->reloadAllBTTVChannelEmotes();
		},
		false);
	GLOBAL_STATE->settings.enableFFZChannelEmotes.connect(
		[] {
			dynamic_cast<chatterino::TwitchIrcServer *>(GLOBAL_STATE->app.getTwitch())
				->reloadAllFFZChannelEmotes();
		},
		false);
	GLOBAL_STATE->settings.enableSevenTVChannelEmotes.connect(
		[] {
			dynamic_cast<chatterino::TwitchIrcServer *>(GLOBAL_STATE->app.getTwitch())
				->reloadAllSevenTVChannelEmotes();
		},
		false);

	obs_frontend_add_save_callback(
		+[](obs_data_t *save_data, bool saving, void *user) {
			if (!GLOBAL_STATE) {
				return;
			}

			if (saving) {
				GLOBAL_STATE->save(save_data);
			} else {
				GLOBAL_STATE->load(save_data);
			}
		},
		nullptr);

	QAction *action =
		static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("New Chatterino Window")));
	QAction::connect(action, &QAction::triggered, [] {
		if (!GLOBAL_STATE) {
			return;
		}
		GLOBAL_STATE->addChat();
	});
}
} // namespace
