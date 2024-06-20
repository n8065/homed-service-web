#include <QFile>
#include <QRandomGenerator>
#include "controller.h"
#include "logger.h"

Controller::Controller(const QString &configFile) : HOMEd(configFile), m_database(new Database(getConfig(), this)), m_tcpServer(new QTcpServer(this)), m_webSocket(new QWebSocketServer("HOMEd", QWebSocketServer::NonSecureMode, this))
{
    logInfo << "Starting version" << SERVICE_VERSION;
    logInfo << "Configuration file is" << getConfig()->fileName();

    m_frontend = getConfig()->value("server/frontend", "/usr/share/homed-web").toString();
    m_username = getConfig()->value("server/username").toString();
    m_password = getConfig()->value("server/password").toString();

    m_auth = m_username.isEmpty() || m_password.isEmpty() ? false : true;

    m_retained = {"device", "expose", "service", "status"};
    m_types = {"css", "js", "json", "png", "svg", "woff2"};

    connect(m_database, &Database::statusUpdated, this, &Controller::statusUpdated);
    connect(m_tcpServer, &QTcpServer::newConnection, this, &Controller::socketConnected);
    connect(m_webSocket, &QWebSocketServer::newConnection, this, &Controller::clientConnected);

    m_database->init();
    m_tcpServer->listen(QHostAddress::Any, static_cast <quint16> (getConfig()->value("server/port", 8080).toInt()));
}

void Controller::httpResponse(QTcpSocket *socket, quint16 code, const QMap <QString, QString> &headers, const QByteArray &response)
{
    QByteArray data;

    switch (code)
    {
       case 200: data = "HTTP/1.1 200 OK"; break;
       case 301: data = "HTTP/1.1 301 Moved Permanently"; break;
       case 404: data = "HTTP/1.1 404 Not Found"; break;
       case 405: data = "HTTP/1.1 405 Method Not Allowed"; break;
       case 500: data = "HTTP/1.1 500 Internal Server Error"; break;
    }

    for (auto it = headers.begin(); it != headers.end(); it++)
        data.append(QString("\r\n%1: %2").arg(it.key(), it.value()).toUtf8());

    socket->write(data.append("\r\n\r\n").append(response));
    socket->close();
}

void Controller::fileResponse(QTcpSocket *socket, const QString &fileName)
{
    QFile file(QString(m_frontend).append(fileName));
    QByteArray type, data;

    if (!file.exists())
    {
        httpResponse(socket, 404);
        return;
    }

    if (!file.open(QFile::ReadOnly))
    {
        httpResponse(socket, 500);
        return;
    }

    switch (m_types.indexOf(fileName.mid(fileName.lastIndexOf('.') + 1)))
    {
        case 0:  type = "text/css"; break;         // css
        case 1:  type = "text/javascript"; break;  // js
        case 2:  type = "application/json"; break; // json
        case 3:  type = "image/png"; break;        // png
        case 4:  type = "image/svg+xml"; break;    // svg
        case 5:  type = "font/woff2"; break;       // woff2
        default: type = "text/html"; break;
    }

    data = file.readAll();

    if (fileName == "/index.html")
        data = QString(data).arg(SERVICE_VERSION, m_auth ? "<span id=\"logout\"><i class=\"icon-enable\"></i> LOGOUT</span>" : QString()).toUtf8();

    httpResponse(socket, 200, {{"Content-Type", type}, {"Content-Length", QString::number(data.length())}}, data);
    file.close();
}

void Controller::quit(void)
{
    m_webSocket->close();

    for (auto it = m_clients.begin(); it != m_clients.end(); it++)
        it.key()->deleteLater();

    HOMEd::quit();
}

void Controller::mqttConnected(void)
{
    mqttSubscribe(mqttTopic("command/web"));

    for (auto it = m_clients.begin(); it != m_clients.end(); it++)
        for (int i = 0; i < it.value().count(); i++)
            mqttSubscribe(mqttTopic(it.value().at(i)));

    m_database->store();
    mqttPublishStatus();
}

void Controller::mqttReceived(const QByteArray &message, const QMqttTopicName &topic)
{
    QString subTopic = topic.name().replace(mqttTopic(), QString());
    QJsonObject json = QJsonDocument::fromJson(message).object();

    if (subTopic == "command/web" && json.value("action").toString() == "updateDashboards")
    {
        m_database->update(json.value("data").toArray());
        m_database->store(true);
        return;
    }

    if (m_retained.contains(subTopic.split('/').value(0)))
        m_messages.insert(subTopic, message);

    for (auto it = m_clients.begin(); it != m_clients.end(); it++)
    {
        if (!it.value().contains(subTopic))
            continue;

        it.key()->sendTextMessage(QJsonDocument({{"topic", subTopic}, {"message", json.isEmpty() ? QJsonValue::Null : QJsonValue(json)}}).toJson(QJsonDocument::Compact));
    }
}

void Controller::statusUpdated(const QJsonObject &json)
{
    mqttPublish(mqttTopic("status/web"), json, true);
}

void Controller::socketConnected(void)
{
    QTcpSocket *socket = m_tcpServer->nextPendingConnection();
    connect(socket, &QTcpSocket::disconnected, this, &Controller::socketDisconnected);
    connect(socket, &QTcpSocket::readyRead, this, &Controller::readyRead);
    m_sockets.push_back(socket);
}

void Controller::socketDisconnected(void)
{
    QTcpSocket *socket = reinterpret_cast <QTcpSocket*> (sender());
    m_sockets.removeAll(socket);
    socket->deleteLater();
}

void Controller::readyRead(void)
{
    QTcpSocket *socket = reinterpret_cast <QTcpSocket*> (sender());
    QByteArray request = socket->peek(socket->bytesAvailable());
    QList <QString> list = QString(request).split("\r\n\r\n"), head = list.value(0).split("\r\n"), target = head.value(0).split(' '), cookieList, itemList;
    QString method = target.value(0), url = target.value(1), content = list.value(1);
    QMap <QString, QString> headers, cookies, items;

    disconnect(socket, &QTcpSocket::readyRead, this, &Controller::readyRead);

    for (int i = 1; i < head.count(); i++)
    {
        QList <QString> header = head.at(i).split(':');
        headers.insert(header.value(0).trimmed(), header.value(1).trimmed());
    }

    cookieList = headers.value("Cookie").split(';');

    for (int i = 0; i < cookieList.count(); i++)
    {
        QList <QString> cookie = cookieList.at(i).split('=');
        cookies.insert(cookie.value(0).trimmed(), cookie.value(1).trimmed());
    }

    if (method == "POST" && headers.value("Content-Length").toInt() > content.length())
    {
        socket->read(request.length());
        socket->waitForReadyRead();
        content.append(socket->readAll());
    }

    itemList = QString(method == "GET" && url.contains('?') ? url.mid(url.indexOf('?') + 1) : content).split('&');

    for (int i = 0; i < itemList.count(); i++)
    {
        QList <QString> item = itemList.at(i).split('=');
        items.insert(item.value(0), QUrl::fromPercentEncoding(item.value(1).toUtf8()));
    }

    if (m_auth && !m_database->tokens().contains(cookies.value("homed-auth-token")) && url != "/manifest.json" && !url.startsWith("/css/") && !url.startsWith("/font/") && !url.startsWith("/img/"))
    {
        if (method == "POST" && items.value("username") == m_username && items.value("password") == m_password)
        {
            QByteArray buffer;
            QString token;

            for (int i = 0; i < 32; i++)
                buffer.append(static_cast <char> (QRandomGenerator::global()->generate()));

            token = buffer.toHex();
            httpResponse(socket, 301, {{"Location", QString(headers.value("X-Ingress-Path")).append("/")}, {"Cache-Control", "no-cache, no-store"}, {"Set-Cookie", QString("homed-auth-token=%1; path=/; max-age=%2").arg(token).arg(COOKIE_MAX_AGE)}});
            m_database->tokens().insert(token);
            m_database->store(true);
        }
        else
            fileResponse(socket, "/login.html");

        return;
    }

    url = url.mid(0, url.indexOf('?'));

    if (url == "/logout")
    {
        httpResponse(socket, 301, {{"Location", QString(headers.value("X-Ingress-Path")).append("/")}, {"Cache-Control", "no-cache, no-store"}, {"Set-Cookie", "homed-auth-token=deleted; path=/; max-age=0"}});

        if (items.value("session") == "all")
        {
            for (auto it = m_clients.begin(); it != m_clients.end(); it++)
                it.key()->deleteLater();

            m_database->tokens().clear();
        }
        else
            m_database->tokens().remove(cookies.value("homed-auth-token"));

        m_database->store(true);
        return;
    }

    if (method != "GET")
    {
        httpResponse(socket, 405);
        return;
    }

    if (headers.value("Upgrade") == "websocket")
    {
        m_webSocket->handleConnection(socket);
        return;
    }

    fileResponse(socket, url != "/" ? url : "/index.html");
}

void Controller::clientConnected(void)
{
    QWebSocket *client = m_webSocket->nextPendingConnection();
    connect(client, &QWebSocket::disconnected, this, &Controller::clientDisconnected);
    connect(client, &QWebSocket::textMessageReceived, this, &Controller::textMessageReceived);
    m_clients.insert(client, QStringList());
}

void Controller::clientDisconnected(void)
{
    QWebSocket *client = reinterpret_cast <QWebSocket*> (sender());
    m_clients.remove(client);
    client->deleteLater();
}

void Controller::textMessageReceived(const QString &message)
{
    auto it = m_clients.find(reinterpret_cast <QWebSocket*> (sender()));
    QJsonObject json = QJsonDocument::fromJson(message.toUtf8()).object();
    QString action = json.value("action").toString(), subTopic = json.value("topic").toString();

    if (it == m_clients.end() || subTopic.isEmpty())
        return;

    if (action == "subscribe")
    {
        if (!it.value().contains(subTopic))
            it.value().push_back(subTopic);

        if (m_messages.contains(subTopic))
            it.key()->sendTextMessage(QJsonDocument({{"topic", subTopic}, {"message", QJsonDocument::fromJson(m_messages.value(subTopic)).object()}}).toJson(QJsonDocument::Compact));

        mqttSubscribe(mqttTopic(subTopic));
    }
    else if (action == "publish")
        mqttPublish(mqttTopic(subTopic), json.value("message").toObject());
    else if (action == "unsubscribe")
        it.value().removeAll(subTopic);
}
