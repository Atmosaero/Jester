#include "ui/widgets/localized_ui.h"
#include "automation/editor_api.h"
#include "ui/widgets/icons.h"
#include "settings/blue_jay_style.h"
#include "automation/mcp_server.h"
#include "ui/window/window.h"
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QSettings>
#include <QStatusBar>
#include <QToolButton>
#include <QVBoxLayout>
namespace trickster {
void Window::buildAutomation() {
    mcp = new McpServer(this);
    auto menu = addTranslatedMenu(menuBar(),"&Automation");
    menu->setObjectName("automationMenu");
    auto start = addTranslatedAction(menu,"Start MCP server");
    start->setObjectName("mcpStartAction");
    auto stop = addTranslatedAction(menu,"Stop MCP server");
    stop->setObjectName("mcpStopAction");
    menu->addSeparator();
    auto connection = addTranslatedAction(menu,"MCP connection…");
    connection->setObjectName("mcpConnectionAction");
    auto copy = addTranslatedAction(menu,"Copy MCP client configuration");
    copy->setObjectName("mcpCopyConfigAction");
    auto status = new QToolButton(this);
    status->setObjectName("mcpStatusButton");
    bindUiText(status,"toolTip","MCP automation server · connection and controls");
    status->setAutoRaise(true);
    statusBar()->addPermanentWidget(status);
    auto update = [this, start, stop, copy, status] {
        bool on = mcp->isRunning();
        start->setEnabled(!on);
        stop->setEnabled(on);
        copy->setEnabled(on);
        status->setText(on ? uiText("MCP :%1").arg(mcp->port()) : uiText("MCP off"));
        status->setStyleSheet(on?QString("color:%1;").arg(BlueJay::Success):QString("color:palette(placeholder-text);"));
    };
    connect(mcp, &McpServer::statusChanged, this, update);
    connect(&LocalizationService::instance(),&LocalizationService::languageChanged,this,update);
    update();
    connect(start, &QAction::triggered, this, [this] {
        auto port = quint16(QSettings().value("automation/port", 18530).toUInt());
        if (mcp->start(port))
            report(uiText("MCP running at ") + mcp->endpoint());
        else
            report(uiText("MCP could not start: ") + mcp->lastError());
    });
    connect(stop, &QAction::triggered, this, [this] {
        mcp->stop();
        report(uiText("MCP stopped. Remote editor commands are disconnected."));
    });
    connect(copy, &QAction::triggered, this, [this] {
        if (mcp->isRunning()) {
            QApplication::clipboard()->setText(
                QString::fromUtf8(QJsonDocument(mcp->connectionConfig()).toJson(QJsonDocument::Indented)));
            report(uiText("MCP client configuration copied, including the local access token."));
        }
    });
    connect(status, &QToolButton::clicked, connection, &QAction::trigger);
    connect(connection, &QAction::triggered, this, [this, start, stop, copy] {
        QDialog dialog(this);
        dialog.setObjectName("mcpConnectionDialog");
        bindUiText(&dialog,"windowTitle","Jester · MCP connection");
        dialog.resize(620, 470);
        auto layout = new QVBoxLayout(&dialog);
        auto intro = translatedWidget<QLabel>("Allow an assistant to control this running editor through MCP. The server listens on this "
            "computer only and starts when you enable it. Pose edits use the editor's Undo history.");
        intro->setWordWrap(true);
        layout->addWidget(intro);
        auto form = new QFormLayout;
        auto port = new QSpinBox(&dialog);
        port->setObjectName("mcpPort");
        port->setRange(1024, 65535);
        port->setValue(QSettings().value("automation/port", 18530).toInt());
        addTranslatedRow(form,"Port", port);
        auto endpoint = new QLineEdit(&dialog);
        endpoint->setObjectName("mcpEndpoint");
        endpoint->setReadOnly(true);
        addTranslatedRow(form,"Streamable HTTP", endpoint);
        auto token = new QLineEdit(mcp->accessToken(), &dialog);
        token->setObjectName("mcpAccessToken");
        token->setEchoMode(QLineEdit::Password);
        token->setReadOnly(true);
        addTranslatedRow(form,"Bearer token", token);
        layout->addLayout(form);
        auto row = new QHBoxLayout;
        auto startButton = translatedWidget<QPushButton>("Start", &dialog);
        startButton->setObjectName("mcpStartButton");
        row->addWidget(startButton);
        auto stopButton = translatedWidget<QPushButton>("Stop", &dialog);
        stopButton->setObjectName("mcpStopButton");
        row->addWidget(stopButton);
        auto copyButton = translatedWidget<QPushButton>("Copy JSON", &dialog);
        copyButton->setObjectName("mcpCopyConfigButton");
        row->addWidget(copyButton);
        auto codexButton = translatedWidget<QPushButton>("Copy Codex config", &dialog);
        codexButton->setObjectName("mcpCopyCodexButton");
        row->addWidget(codexButton);
        connect(codexButton, &QPushButton::clicked, &dialog,
                [this] { QApplication::clipboard()->setText(mcp->codexConfig()); });
        layout->addLayout(row);
        auto summary = new QLabel(&dialog);
        summary->setObjectName("mcpSummary");
        layout->addWidget(summary);
        auto activity = new QPlainTextEdit(&dialog);
        activity->setObjectName("mcpActivity");
        activity->setReadOnly(true);
        activity->setMaximumBlockCount(100);
        bindPlaceholder(activity,"Tool calls will appear here. Arguments and access tokens are not logged.");
        layout->addWidget(activity, 1);
        auto refresh = [&, this] {
            bool on = mcp->isRunning();
            port->setEnabled(!on);
            if (on)
                port->setValue(mcp->port());
            startButton->setEnabled(!on);
            stopButton->setEnabled(on);
            copyButton->setEnabled(on);
            codexButton->setEnabled(on);
            endpoint->setText(on ? mcp->endpoint() : uiText("Stopped"));
            summary->setText(uiText("%1 tools · %2 resources · %3 requests%4")
                                 .arg(mcp->api()->tools().size())
                                 .arg(mcp->api()->resources().size())
                                 .arg(mcp->requestCount())
                                 .arg(mcp->lastError().isEmpty() ? QString() : " · " + mcp->lastError()));
        };
        connect(mcp, &McpServer::statusChanged, &dialog, refresh);
        connect(&LocalizationService::instance(),&LocalizationService::languageChanged,&dialog,refresh);
        connect(mcp, &McpServer::activity, &dialog, [activity](QString method, bool success) {
            activity->appendPlainText((success ? "OK  " : "ERR ") + method);
        });
        connect(startButton, &QPushButton::clicked, &dialog, [port, start] {
            QSettings().setValue("automation/port", port->value());
            start->trigger();
        });
        connect(stopButton, &QPushButton::clicked, stop, &QAction::trigger);
        connect(copyButton, &QPushButton::clicked, copy, &QAction::trigger);
        auto close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        layout->addWidget(close);
        connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        refresh();
        dialog.exec();
    });
    mcp->api()->nameActions();
}
} // namespace trickster
