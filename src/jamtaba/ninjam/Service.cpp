#include "Service.h"
#include "Server.h"
#include "User.h"
#include "protocol/ServerMessageParser.h"
#include "protocol/ServerMessages.h"
#include "protocol/ClientMessages.h"
#include <algorithm>
#include <QHostAddress>
#include <QDateTime>
#include <QDataStream>

#include <QThread>

#include <cassert>

using namespace Ninjam;

std::unique_ptr<Service> Service::serviceInstance;

const QStringList Service::botNames = buildBotNamesList();

Service::Service()
    :
      lastSendTime(0),
//      running(false),
      initialized(false)

{
    connect(&socket, SIGNAL(readyRead()), this, SLOT(socketReadSlot()));
    connect(&socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(socketErrorSlot(QAbstractSocket::SocketError)));
    connect(&socket, SIGNAL(disconnected()), this, SLOT(socketDisconnectSlot()));
    connect(&socket, SIGNAL(connected()), this, SLOT(socketConnectedSlot()));
}

Service::~Service(){
    qDebug() << "NinjamService destructor";
    disconnect(&socket, SIGNAL(readyRead()), this, SLOT(socketReadSlot()));
    disconnect(&socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(socketErrorSlot(QAbstractSocket::SocketError)));
    disconnect(&socket, SIGNAL(disconnected()), this, SLOT(socketDisconnectSlot()));
    disconnect(&socket, SIGNAL(connected()), this, SLOT(socketConnectedSlot()));

    if(socket.isOpen()){
        socket.disconnectFromHost();
    }
}

void Service::sendAudioIntervalPart(QByteArray GUID, QByteArray encodedAudioBuffer, bool isLastPart){
    if(!initialized){
        return;
    }
    ClientIntervalUploadWrite msg(GUID, encodedAudioBuffer, isLastPart);
    sendMessageToServer(&msg);
}

void Service::sendAudioIntervalBegin(QByteArray GUID, quint8 channelIndex){
    if(!initialized){
        return;
    }
    ClientUploadIntervalBegin msg(GUID, channelIndex, this->userName);
    sendMessageToServer(&msg);
}

//void Service::stopTransmitting(char* GUID[], quint8 userChannel){
//    enqueueMessageToSend(new ClientUploadIntervalBegin(userChannel, newUserName));
//}


//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
void Service::socketReadSlot(){
    if(socket.bytesAvailable() < 5){
        qDebug() << "not have enough bytes to read message header (5 bytes)";
        return;
    }

    QDataStream stream(&socket);
    stream.setByteOrder(QDataStream::LittleEndian);

    static quint8 messageTypeCode;
    static quint32 payloadLenght;
    static bool lastMessageWasIncomplete = false;
    while (socket.bytesAvailable() >= 5){//consume all messages
        if(!lastMessageWasIncomplete){
            stream >> messageTypeCode >> payloadLenght;
        }
        if (socket.bytesAvailable() >= (int)payloadLenght) {//message payload is available to read
            lastMessageWasIncomplete = false;
            const Ninjam::ServerMessage& message = ServerMessageParser::parse(static_cast<ServerMessageType>(messageTypeCode), stream, payloadLenght) ;
            //qDebug() << message;
            invokeMessageHandler(message);
            if(needSendKeepAlive()){
                ClientKeepAlive clientKeepAliveMessage;
                sendMessageToServer((ClientMessage*)&clientKeepAliveMessage);
            }
        }
        else{
            //qWarning() << "incomplete message!";
            lastMessageWasIncomplete = true;
            break;
        }
    }
}

void Service::socketErrorSlot(QAbstractSocket::SocketError e)
{
    Q_UNUSED(e);
    currentServer.reset(nullptr);
    emit error(socket.errorString());
}

void Service::socketConnectedSlot(){
    //the old current server is deleted by unique_ptr
    QString serverIp = socket.peerName();
    quint16 serverPort = socket.peerPort();
    currentServer.reset( new Server(serverIp, serverPort));
}

void Service::socketDisconnectSlot(){
    this->initialized = false;
    emit disconnectedFromServer(*currentServer);
}


Service* Service::getInstance() {
    if(!serviceInstance){
        serviceInstance = std::unique_ptr<Service>(new Service());
    }
    return serviceInstance.get();
}

bool Service::isBotName(QString userName) {
    userName = userName.trimmed();
    return botNames.contains(userName);
}

QStringList Service::buildBotNamesList(){
    QStringList names;
    names.append("Jambot");
    names.append("ninjamer.com");
    names.append("ninbot");
    names.append("ninbot.com");
    names.append("MUTANTLAB");
    names.append("LiveStream");
    names.append("localhost");
    return names;
}

QString Service::getConnectedUserName() {
    if (initialized) {
        return newUserName;
    }
    qCritical() << "not initialized, newUserName is not available!";
    return "";
}

float Service::getIntervalPeriod() {
    if (currentServer ) {
        return (float)60000 / currentServer->getBpm() * currentServer->getBpi();
    }
    return 0;
}


void Service::voteToChangeBPI(int newBPI){
    QString text = "!vote bpi " + QString::number(newBPI);
    ChatMessage message(text);
    sendMessageToServer(&message);
}

void Service::voteToChangeBPM(int newBPM){
    QString text = "!vote bpm " + QString::number(newBPM);
    ChatMessage message(text);
    sendMessageToServer(&message);
}

void Service::sendChatMessageToServer(QString message){
    ChatMessage msg(message);
    sendMessageToServer(&msg);
}

void Service::sendMessageToServer(ClientMessage *message){
    QByteArray outBuffer;
    message->serializeTo(outBuffer);

    int bytesWrited = socket.write(outBuffer);
    if(bytesWrited > 0){
        socket.flush();
        lastSendTime = QDateTime::currentMSecsSinceEpoch();
    }
    else{
        qCritical("não escreveu os bytes");
    }

    if((int)message->getPayload() + 5 != outBuffer.size()){
        qWarning() << "(int)message->getPayload() + 5: " << ((int)message->getPayload() + 5) << "outbuffer.size():" << outBuffer.size();
    }

}

bool Service::needSendKeepAlive() const{
    long ellapsedSeconds = (int)(QDateTime::currentMSecsSinceEpoch() - lastSendTime)/1000;
    return ellapsedSeconds >= serverKeepAlivePeriod;
}

void Service::handle(const UserInfoChangeNotifyMessage& msg) {
    //QMap<NinjamUser*, QList<NinjamUserChannel*>> allUsersChannels = msg.getUsersChannels();
    QSet<QString> users = QSet<QString>::fromList( msg.getUsersNames());
    foreach (QString userFullName , users) {
        if (!currentServer->containsUser(userFullName)) {
            User newUser(userFullName);
            currentServer->addUser( newUser );
            emit userEnterInTheJam(newUser);
        }
        handleUserChannels(userFullName, msg.getUserChannels(userFullName));
    }

    ClientSetUserMask setUserMask(msg.getUsersNames());
    sendMessageToServer( &setUserMask );//enable new users channels
}

void Service::handle(const DownloadIntervalBegin& msg){
    if (!msg.downloadShouldBeStopped() && msg.isValidOggDownload()) {
        quint8 channelIndex = msg.getChannelIndex();
        QString userFullName = msg.getUserName();
        QString GUID = msg.getGUID();
        downloads.insert(GUID, new Download(userFullName, channelIndex, GUID));
    }

}

void Service::handle(const DownloadIntervalWrite& msg){
    if (downloads.contains(msg.getGUID())) {
        Download* download = downloads[msg.getGUID()];
        download->appendVorbisData(msg.getEncodedAudioData());
        User* user = currentServer->getUser(download->getUserFullName());
        if (msg.downloadIsComplete()) {
            emit audioIntervalCompleted(*user, download->getChannelIndex(), download->getVorbisData());
            delete download;
            downloads.remove(msg.getGUID());
        }
        else{
            emit audioIntervalDownloading(*user, download->getChannelIndex(), msg.getEncodedAudioData().size());
        }
    } else {
        qCritical("GUID is not in map!");
    }
}

void Service::handle(const ServerKeepAliveMessage& /*msg*/){
    ClientKeepAlive clientKeepAliveMessage;
    sendMessageToServer((ClientMessage*)&clientKeepAliveMessage);
}

void Service::handle(const ServerAuthChallengeMessage& msg){
    ClientAuthUserMessage msgAuthUser(this->userName, msg.getChallenge(), msg.getProtocolVersion());
    sendMessageToServer(&msgAuthUser);
    this->serverLicence = msg.getLicenceAgreement();
    this->serverKeepAlivePeriod = msg.getServerKeepAlivePeriod();
}

void Service::sendNewChannelsListToServer(QStringList channelsNames){
    this->channels = channelsNames;
    ClientSetChannel setChannelMsg(this->channels);
    sendMessageToServer(&setChannelMsg);
}

void Service::sendRemovedChannelIndex(int removedChannelIndex){
    assert( removedChannelIndex >= 0 && removedChannelIndex < channels.size() );
    channels.removeAt(removedChannelIndex);
    ClientSetChannel setChannelMsg(this->channels);
    sendMessageToServer(&setChannelMsg);

}

void Service::handle(const ServerAuthReplyMessage& msg){
    if(msg.userIsAuthenticated()){
        ClientSetChannel setChannelMsg(this->channels);
        sendMessageToServer(&setChannelMsg);

    }
    //when user is not authenticated the socketErrorSlot is called and dispatch an error signal
//    else{
//        emit error(msg.getErrorMessage());
//    }
}

void Service::startServerConnection(QString serverIp, int serverPort, QString userName, QStringList channels, QString password){
    initialized = false;
    this->userName = userName;
    this->password = password;
    this->channels = channels;

    socket.connectToHost(serverIp, serverPort);
}

void Service::disconnectFromServer(){
    socket.disconnectFromHost();
}

void Service::setBpm(quint16 newBpm){
    if (currentServer == nullptr) {
        throw ("currentServer == null");
    }
    if (currentServer->setBpm(newBpm) && initialized) {

        emit serverBpmChanged(currentServer->getBpm());

    }
}

void Service::setBpi(quint16 bpi) {
    if (currentServer == nullptr) {
        throw ("currentServer == null");
    }
    quint16 lastBpi = currentServer->getBpi();
    if (currentServer->setBpi(bpi) && initialized) {

        emit serverBpiChanged(currentServer->getBpi(), lastBpi);

    }
}

//+++++++++++++ SERVER MESSAGE HANDLERS +++++++++++++=
void Service::handleUserChannels(QString userFullName, QList<UserChannel> channelsInTheServer) {
    //check for new channels
    User* user = this->currentServer->getUser(userFullName);
    foreach (UserChannel c , channelsInTheServer) {
        if (c.isActive()) {
            if (!user->hasChannel(c.getIndex())) {
                user->addChannel(c);
                emit userChannelCreated(*user, c);

            } else {//check for channel updates
                if (user->hasChannels()) {
                    //UserChannel userChannel = user->getChannel(c.getIndex());
                    if (channelIsOutdate(*user, c)) {
                        user->setChannelName(c.getIndex(), c.getName());
                        user->setChannelFlags(c.getIndex(), c.getFlags());
                        emit userChannelUpdated(*user, user->getChannel(c.getIndex()));
                    }
                }
            }
        } else {
            user->removeChannel(c.getIndex());
            emit userChannelRemoved(*user, c);

        }
    }
}

//verifica se o canal do usuario esta diferente do canal que veio do servidor
bool Service::channelIsOutdate(const User& user, const UserChannel& serverChannel) {
    if (user.getFullName() != serverChannel.getUserFullName()) {
        throw ("The user in channel is illegal!");
    }
    UserChannel userChannel = user.getChannel(serverChannel.getIndex());
    if (userChannel.getName() != serverChannel.getName()) {
        return true;
    }
    if (userChannel.getFlags() != serverChannel.getFlags()) {
        return true;
    }
    return false;
}

//+++++++++++++ CHAT MESSAGES ++++++++++++++++++++++

void Service::handle(const ServerChatMessage& msg) {
    switch (msg.getCommand()) {
    case ChatCommandType::JOIN:
        //
        break;
    case ChatCommandType::MSG:
    {
        QString messageSender = msg.getArguments().at(0);
        QString messageText = msg.getArguments().at(1);

        emit chatMessageReceived(User(messageSender), messageText);

        break;

    }
    case ChatCommandType::PART:
    {
        QString userLeavingTheServer = msg.getArguments().at(0);

        emit userLeaveTheJam(User(userLeavingTheServer));

        break;
    }
    case ChatCommandType::PRIVMSG:
    {
        QString messageSender = msg.getArguments().at(0);
        QString messageText = msg.getArguments().at(1);

        emit privateMessageReceived(User(messageSender), messageText);


        break;
    }
    case ChatCommandType::TOPIC:
    {
        //QString userName = msg.getArguments().at(0);
        QString topicText = msg.getArguments().at(1);
        if (!initialized) {
            initialized = true;
            currentServer->setTopic(topicText);
            currentServer->setLicence(serverLicence);//server licence is received when the hand shake with server is started
            //serverLicence.clear();
            emit connectedInServer(*currentServer);
            emit chatMessageReceived(Ninjam::User(currentServer->getHostName()), topicText);
        }
        break;
    }
    case ChatCommandType::USERCOUNT:
    {
        int users = msg.getArguments().at(0).toInt();
        int maxUsers = msg.getArguments().at(1).toInt();

        emit userCountMessageReceived(users, maxUsers);

        break;
    }
    default:
        qCritical("chat message type not implemented");
    }
}


void Service::handle(const ServerConfigChangeNotifyMessage& msg)
{
    quint16 bpi = msg.getBpi();
    quint16 bpm = msg.getBpm();
    if (bpi != currentServer->getBpi()) {
        setBpi(bpi);
    }
    if (bpm != currentServer->getBpm()) {
        setBpm(bpm);
    }
}

void Service::invokeMessageHandler(const ServerMessage& message){
    //qDebug() << message;
    switch (message.getMessageType()) {
    case ServerMessageType::AUTH_CHALLENGE:
        handle((ServerAuthChallengeMessage&)message);
        break;
    case ServerMessageType::AUTH_REPLY:
        handle((ServerAuthReplyMessage&)message);
        break;
    case ServerMessageType::SERVER_CONFIG_CHANGE_NOTIFY:
        handle((ServerConfigChangeNotifyMessage&) message);
        break;
    case ServerMessageType::USER_INFO_CHANGE_NOTIFY:
        handle((UserInfoChangeNotifyMessage&) message);
        break;
    case ServerMessageType::CHAT_MESSAGE:
        handle((ServerChatMessage&) message);
        break;
    case ServerMessageType::KEEP_ALIVE:
        handle((ServerKeepAliveMessage&) message);
        break;
    case ServerMessageType::DOWNLOAD_INTERVAL_BEGIN:
        handle((DownloadIntervalBegin&) message);
        break;
    case ServerMessageType::DOWNLOAD_INTERVAL_WRITE:
        handle((DownloadIntervalWrite&) message);
        break;

    default:
        qCritical("receive a not implemented yet message!");
    }
}

QString Service::getCurrentServerLicence() const{
    return serverLicence;
}



/*
//++++++

    public void close() {
        publicServersParser.shutdown();
        publicServersParser = null;
        LOGGER.info("Closing ninjamservice...");
        closeSocketChannel();
        if (serviceThread != null) {
            LOGGER.info("closing NinjamService serviceThread...");
            serviceThread.shutdownNow();
            serviceThread = null;
            LOGGER.info("NinjamService serviceThread closed!");
        }
        LOGGER.info("NInjaM service closed sucessfull!");

    }

    public void addListener(NinjaMServiceListener l) {
        synchronized (listeners) {
            if (!listeners.contains(l)) {
                listeners.add(l);
                //System.out.println("listener " + l.getClass().getName() + " added in NinjamService");
                //for (NinjaMServiceListener lstnr : listeners) {
                //  System.out.println("\t" + lstnr.getClass().getName());
                //}
            }
            //else{
            //  LOGGER.log(Level.SEVERE, "listener {0} not added in NinjamService!", l.getClass().getName());
            //}
        }
    }

    public void removeListener(NinjaMServiceListener l) {
        synchronized (listeners) {
            listeners.remove(l);
            //System.out.println("listener " + l.getClass().getName() + " removed from NinjamService");
        }
    }



    public static byte[] newGUID() {

        UUID id = UUID.randomUUID();
        ByteBuffer b = ByteBuffer.wrap(new byte[16]).order(ByteOrder.LITTLE_ENDIAN);
        b.putLong(id.getMostSignificantBits());
        b.putLong(id.getLeastSignificantBits());
        return b.array();
//        String idString = Long.toHexString(id.getLeastSignificantBits() + id.getMostSignificantBits()).toUpperCase();
//        if (idString.length() < 16) {
//            int diff = 16 - idString.length();
//            byte bytes[] = new byte[16];
//            byte stringBytes[] = idString.getBytes();
//            System.arraycopy(stringBytes, 0, bytes, diff, stringBytes.length);
//            for (int i = 0; i < diff; i++) {
//                bytes[i] = (byte) 'A';
//            }
//            return bytes;
//
//        }

        //return idString.getBytes();
    }
*/

