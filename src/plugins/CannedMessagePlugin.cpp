#include "configuration.h"
#include "CannedMessagePlugin.h"
#include "MeshService.h"
#include "FSCommon.h"
#include "mesh/generated/cannedmessages.pb.h"

// TODO: reuse defined from Screen.cpp
#define FONT_SMALL ArialMT_Plain_10
#define FONT_MEDIUM ArialMT_Plain_16
#define FONT_LARGE ArialMT_Plain_24

// Remove Canned message screen if no action is taken for some milliseconds
#define INACTIVATE_AFTER_MS 20000

static const char *cannedMessagesConfigFile = "/prefs/cannedConf.proto";

CannedMessagePluginConfig cannedMessagePluginConfig;

CannedMessagePlugin *cannedMessagePlugin;

// TODO: move it into NodeDB.h!
extern bool loadProto(const char *filename, size_t protoSize, size_t objSize, const pb_msgdesc_t *fields, void *dest_struct);
extern bool saveProto(const char *filename, size_t protoSize, size_t objSize, const pb_msgdesc_t *fields, const void *dest_struct);

CannedMessagePlugin::CannedMessagePlugin()
    : SinglePortPlugin("canned", PortNum_TEXT_MESSAGE_APP),
    concurrency::OSThread("CannedMessagePlugin")
{
    if (radioConfig.preferences.canned_message_plugin_enabled)
    {
        this->loadProtoForPlugin();
        if(this->splitConfiguredMessages() <= 0)
        {
            DEBUG_MSG("CannedMessagePlugin: No messages are configured. Plugin is disabled\n");
            this->runState = CANNED_MESSAGE_RUN_STATE_DISABLED;
        }
        else
        {
            DEBUG_MSG("CannedMessagePlugin is enabled\n");
            this->inputObserver.observe(inputBroker);
        }
    }
}

/**
 * @brief Items in array this->messages will be set to be pointing on the right
 *     starting points of the string this->messageStore
 *
 * @return int Returns the number of messages found.
 */
int CannedMessagePlugin::splitConfiguredMessages()
{
    int messageIndex = 0;
    int i = 0;

    // collect all the message parts
    strcpy(
        this->messageStore,
        cannedMessagePluginConfig.messagesPart1);
    strcat(
        this->messageStore,
        cannedMessagePluginConfig.messagesPart2);
    strcat(
        this->messageStore,
        cannedMessagePluginConfig.messagesPart3);
    strcat(
        this->messageStore,
        cannedMessagePluginConfig.messagesPart4);

    // The first message points to the beginning of the store.
    this->messages[messageIndex++] =
        this->messageStore;
    int upTo =
        strlen(this->messageStore) - 1;

    while (i < upTo)
    {
                 if (this->messageStore[i] == '|')
        {
            // Message ending found, replace it with string-end character.
            this->messageStore[i] = '\0';
            DEBUG_MSG("CannedMessage %d is: '%s'\n",
                messageIndex-1, this->messages[messageIndex-1]);

            // hit our max messages, bail
            if (messageIndex >= CANNED_MESSAGE_PLUGIN_MESSAGE_MAX_COUNT)
            {
                this->messagesCount = messageIndex;
                return this->messagesCount;
            }

            // Next message starts after pipe (|) just found.
            this->messages[messageIndex++] =
                (this->messageStore + i + 1);
        }
        i += 1;
    }
    if (strlen(this->messages[messageIndex-1]) > 0)
    {
        // We have a last message.
        DEBUG_MSG("CannedMessage %d is: '%s'\n",
            messageIndex-1, this->messages[messageIndex-1]);
        this->messagesCount = messageIndex;
    }
    else
    {
        this->messagesCount = messageIndex-1;
    }

    return this->messagesCount;
}

int CannedMessagePlugin::handleInputEvent(const InputEvent *event)
{
    if (
        (strlen(radioConfig.preferences.canned_message_plugin_allow_input_source) > 0) &&
        (strcmp(radioConfig.preferences.canned_message_plugin_allow_input_source, event->source) != 0) &&
        (strcmp(radioConfig.preferences.canned_message_plugin_allow_input_source, "_any") != 0))
    {
        // Event source is not accepted.
        // Event only accepted if source matches the configured one, or
        //   the configured one is "_any" (or if there is no configured
        //   source at all)
        return 0;
    }

    bool validEvent = false;
    if (event->inputEvent == static_cast<char>(InputEventChar_KEY_UP))
    {
        DEBUG_MSG("Canned message event UP\n");
        this->runState = CANNED_MESSAGE_RUN_STATE_ACTION_UP;
        validEvent = true;
    }
    if (event->inputEvent == static_cast<char>(InputEventChar_KEY_DOWN))
    {
        DEBUG_MSG("Canned message event DOWN\n");
        this->runState = CANNED_MESSAGE_RUN_STATE_ACTION_DOWN;
        validEvent = true;
    }
    if (event->inputEvent == static_cast<char>(InputEventChar_KEY_SELECT))
    {
        DEBUG_MSG("Canned message event Select\n");
        this->runState = CANNED_MESSAGE_RUN_STATE_ACTION_SELECT;
        validEvent = true;
    }

    if (validEvent)
    {
        // Let runOnce to be called immediately.
        setIntervalFromNow(0);
    }

    return 0;
}

void CannedMessagePlugin::sendText(NodeNum dest,
      const char* message,
      bool wantReplies)
{
    MeshPacket *p = allocDataPacket();
    p->to = dest;
    p->want_ack = true;
    p->decoded.payload.size = strlen(message);
    memcpy(p->decoded.payload.bytes, message, p->decoded.payload.size);
    if (radioConfig.preferences.canned_message_plugin_send_bell)
    {
        p->decoded.payload.bytes[p->decoded.payload.size-1] = 7; // Bell character
        p->decoded.payload.bytes[p->decoded.payload.size] = '\0'; // Bell character
        p->decoded.payload.size++;
    }

    DEBUG_MSG("Sending message id=%d, msg=%.*s\n",
      p->id, p->decoded.payload.size, p->decoded.payload.bytes);

    service.sendToMesh(p);
}

int32_t CannedMessagePlugin::runOnce()
{
    if ((!radioConfig.preferences.canned_message_plugin_enabled)
        || (this->runState == CANNED_MESSAGE_RUN_STATE_DISABLED)
        || (this->runState == CANNED_MESSAGE_RUN_STATE_INACTIVE))
    {
        return 30000; // TODO: should return MAX_VAL
    }
    DEBUG_MSG("Check status\n");
    UIFrameEvent e = {false, true};
    if (this->runState == CANNED_MESSAGE_RUN_STATE_SENDING_ACTIVE)
    {
        // TODO: might have some feedback of sendig state
        this->runState = CANNED_MESSAGE_RUN_STATE_INACTIVE;
        e.frameChanged = true;
        this->currentMessageIndex = -1;
        this->notifyObservers(&e);
    }
    else if (
        (this->runState == CANNED_MESSAGE_RUN_STATE_ACTIVE)
         && (millis() - this->lastTouchMillis) > INACTIVATE_AFTER_MS)
    {
        // Reset plugin
        DEBUG_MSG("Reset due the lack of activity.\n");
        e.frameChanged = true;
        this->currentMessageIndex = -1;
        this->runState = CANNED_MESSAGE_RUN_STATE_INACTIVE;
        this->notifyObservers(&e);
    }
    else if (this->currentMessageIndex == -1)
    {
        this->currentMessageIndex = 0;
        DEBUG_MSG("First touch (%d):%s\n", this->currentMessageIndex, this->getCurrentMessage());
        e.frameChanged = true;
        this->runState = CANNED_MESSAGE_RUN_STATE_ACTIVE;
    }
    else if (this->runState == CANNED_MESSAGE_RUN_STATE_ACTION_SELECT)
    {
        sendText(
            NODENUM_BROADCAST,
            this->messages[this->currentMessageIndex],
            true);
        this->runState = CANNED_MESSAGE_RUN_STATE_SENDING_ACTIVE;
        this->currentMessageIndex = -1;
        this->notifyObservers(&e);
        return 2000;
    }
    else if (this->runState == CANNED_MESSAGE_RUN_STATE_ACTION_UP)
    {
        this->currentMessageIndex = getPrevIndex();
        this->runState = CANNED_MESSAGE_RUN_STATE_ACTIVE;
        DEBUG_MSG("MOVE UP (%d):%s\n", this->currentMessageIndex, this->getCurrentMessage());
    }
    else if (this->runState == CANNED_MESSAGE_RUN_STATE_ACTION_DOWN)
    {
        this->currentMessageIndex = this->getNextIndex();
        this->runState = CANNED_MESSAGE_RUN_STATE_ACTIVE;
        DEBUG_MSG("MOVE DOWN (%d):%s\n", this->currentMessageIndex, this->getCurrentMessage());
    }

    if (this->runState == CANNED_MESSAGE_RUN_STATE_ACTIVE)
    {
        this->lastTouchMillis = millis();
        this->notifyObservers(&e);
        return INACTIVATE_AFTER_MS;
    }

    return 30000; // TODO: should return MAX_VAL
}

const char* CannedMessagePlugin::getCurrentMessage()
{
    return this->messages[this->currentMessageIndex];
}
const char* CannedMessagePlugin::getPrevMessage()
{
    return this->messages[this->getPrevIndex()];
}
const char* CannedMessagePlugin::getNextMessage()
{
    return this->messages[this->getNextIndex()];
}
bool CannedMessagePlugin::shouldDraw()
{
    if (!radioConfig.preferences.canned_message_plugin_enabled)
    {
        return false;
    }
    return (currentMessageIndex != -1) || (this->runState != CANNED_MESSAGE_RUN_STATE_INACTIVE);
}

int CannedMessagePlugin::getNextIndex()
{
    if (this->currentMessageIndex >= (this->messagesCount -1))
    {
        return 0;
    }
    else
    {
        return this->currentMessageIndex + 1;
    }
}

int CannedMessagePlugin::getPrevIndex()
{
    if (this->currentMessageIndex <= 0)
    {
        return this->messagesCount - 1;
    }
    else
    {
        return this->currentMessageIndex - 1;
    }
}

void CannedMessagePlugin::drawFrame(
    OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    displayedNodeNum = 0; // Not currently showing a node pane

    if (cannedMessagePlugin->runState == CANNED_MESSAGE_RUN_STATE_SENDING_ACTIVE)
    {
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->setFont(FONT_MEDIUM);
        display->drawString(display->getWidth()/2 + x, 0 + y + 12, "Sending...");
    }
    else
    {
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        display->setFont(FONT_SMALL);
        display->drawString(0 + x, 0 + y, cannedMessagePlugin->getPrevMessage());
        display->setFont(FONT_MEDIUM);
        display->drawString(0 + x, 0 + y + 8, cannedMessagePlugin->getCurrentMessage());
        display->setFont(FONT_SMALL);
        display->drawString(0 + x, 0 + y + 24, cannedMessagePlugin->getNextMessage());
    }
}

void CannedMessagePlugin::loadProtoForPlugin()
{
    if (!loadProto(cannedMessagesConfigFile, CannedMessagePluginConfig_size, sizeof(cannedMessagesConfigFile), CannedMessagePluginConfig_fields, &cannedMessagePluginConfig)) {
        installDefaultCannedMessagePluginConfig();
    }
}

/**
 * @brief Save the plugin config to file.
 *
 * @return true On success.
 * @return false On error.
 */
bool CannedMessagePlugin::saveProtoForPlugin()
{
    bool okay = true;

#ifdef FS
    FS.mkdir("/prefs");
#endif

    okay &= saveProto(cannedMessagesConfigFile, CannedMessagePluginConfig_size, sizeof(CannedMessagePluginConfig), CannedMessagePluginConfig_fields, &cannedMessagePluginConfig);

    return okay;
}

/**
 * @brief Fill configuration with default values.
 */
void CannedMessagePlugin::installDefaultCannedMessagePluginConfig()
{
    memset(cannedMessagePluginConfig.messagesPart1, 0, sizeof(cannedMessagePluginConfig.messagesPart1));
    memset(cannedMessagePluginConfig.messagesPart2, 0, sizeof(cannedMessagePluginConfig.messagesPart2));
    memset(cannedMessagePluginConfig.messagesPart3, 0, sizeof(cannedMessagePluginConfig.messagesPart3));
    memset(cannedMessagePluginConfig.messagesPart4, 0, sizeof(cannedMessagePluginConfig.messagesPart4));
}

/**
 * @brief An admin message arrived to AdminPlugin. We are asked whether we want to handle that.
 *
 * @param mp The mesh packet arrived.
 * @param request The AdminMessage request extracted from the packet.
 * @param response The prepared response
 * @return AdminMessageHandleResult HANDLED if message was handled
 *   HANDLED_WITH_RESULT if a result is also prepared.
 */
AdminMessageHandleResult CannedMessagePlugin::handleAdminMessageForPlugin(
        const MeshPacket &mp, AdminMessage *request, AdminMessage *response)
{
    AdminMessageHandleResult result;

    switch (request->which_variant) {
    case AdminMessage_get_canned_message_plugin_part1_request_tag:
        DEBUG_MSG("Client is getting radio canned message part1\n");
        this->handleGetCannedMessagePluginPart1(mp, response);
        result = AdminMessageHandleResult::HANDLED_WITH_RESPONSE;
        break;

    case AdminMessage_get_canned_message_plugin_part2_request_tag:
        DEBUG_MSG("Client is getting radio canned message part2\n");
        this->handleGetCannedMessagePluginPart2(mp, response);
        result = AdminMessageHandleResult::HANDLED_WITH_RESPONSE;
        break;

    case AdminMessage_get_canned_message_plugin_part3_request_tag:
        DEBUG_MSG("Client is getting radio canned message part3\n");
        this->handleGetCannedMessagePluginPart3(mp, response);
        result = AdminMessageHandleResult::HANDLED_WITH_RESPONSE;
        break;

    case AdminMessage_get_canned_message_plugin_part4_request_tag:
        DEBUG_MSG("Client is getting radio canned message part4\n");
        this->handleGetCannedMessagePluginPart4(mp, response);
        result = AdminMessageHandleResult::HANDLED_WITH_RESPONSE;
        break;

    case AdminMessage_set_canned_message_plugin_part1_tag:
        DEBUG_MSG("Client is setting radio canned message part 1\n");
        this->handleSetCannedMessagePluginPart1(
            request->set_canned_message_plugin_part1);
        result = AdminMessageHandleResult::HANDLED;
        break;

    case AdminMessage_set_canned_message_plugin_part2_tag:
        DEBUG_MSG("Client is setting radio canned message part 2\n");
        this->handleSetCannedMessagePluginPart2(
            request->set_canned_message_plugin_part2);
        result = AdminMessageHandleResult::HANDLED;
        break;

    case AdminMessage_set_canned_message_plugin_part3_tag:
        DEBUG_MSG("Client is setting radio canned message part 3\n");
        this->handleSetCannedMessagePluginPart3(
            request->set_canned_message_plugin_part3);
        result = AdminMessageHandleResult::HANDLED;
        break;

    case AdminMessage_set_canned_message_plugin_part4_tag:
        DEBUG_MSG("Client is setting radio canned message part 4\n");
        this->handleSetCannedMessagePluginPart4(
            request->set_canned_message_plugin_part4);
        result = AdminMessageHandleResult::HANDLED;
        break;

    default:
        result = AdminMessageHandleResult::NOT_HANDLED;
    }

    return result;
}

void CannedMessagePlugin::handleGetCannedMessagePluginPart1(
    const MeshPacket &req, AdminMessage *response)
{
    DEBUG_MSG("*** handleGetCannedMessagePluginPart1\n");
    assert(req.decoded.want_response);

    response->which_variant = AdminMessage_get_canned_message_plugin_part1_response_tag;
    strcpy(
        response->get_canned_message_plugin_part1_response,
        cannedMessagePluginConfig.messagesPart1);
}

void CannedMessagePlugin::handleGetCannedMessagePluginPart2(
    const MeshPacket &req, AdminMessage *response)
{
    DEBUG_MSG("*** handleGetCannedMessagePluginPart2\n");
    assert(req.decoded.want_response);

    response->which_variant = AdminMessage_get_canned_message_plugin_part2_response_tag;
    strcpy(
        response->get_canned_message_plugin_part2_response,
        cannedMessagePluginConfig.messagesPart2);
}

void CannedMessagePlugin::handleGetCannedMessagePluginPart3(
    const MeshPacket &req, AdminMessage *response)
{
    DEBUG_MSG("*** handleGetCannedMessagePluginPart3\n");
    assert(req.decoded.want_response);

    response->which_variant = AdminMessage_get_canned_message_plugin_part3_response_tag;
    strcpy(
        response->get_canned_message_plugin_part3_response,
        cannedMessagePluginConfig.messagesPart3);
}

void CannedMessagePlugin::handleGetCannedMessagePluginPart4(
    const MeshPacket &req, AdminMessage *response)
{
    DEBUG_MSG("*** handleGetCannedMessagePluginPart4\n");
    assert(req.decoded.want_response);

    response->which_variant = AdminMessage_get_canned_message_plugin_part4_response_tag;
    strcpy(
        response->get_canned_message_plugin_part4_response,
        cannedMessagePluginConfig.messagesPart4);
}

void CannedMessagePlugin::handleSetCannedMessagePluginPart1(const char *from_msg)
{
    int changed = 0;

    if (*from_msg)
    {
        changed |= strcmp(cannedMessagePluginConfig.messagesPart1, from_msg);
        strcpy(cannedMessagePluginConfig.messagesPart1, from_msg);
        DEBUG_MSG("*** from_msg.text:%s\n", from_msg);
    }

    if (changed)
    {
        this->saveProtoForPlugin();
    }
}

void CannedMessagePlugin::handleSetCannedMessagePluginPart2(const char *from_msg)
{
    int changed = 0;

    if (*from_msg)
    {
        changed |= strcmp(cannedMessagePluginConfig.messagesPart2, from_msg);
        strcpy(cannedMessagePluginConfig.messagesPart2, from_msg);
    }

    if (changed)
    {
        this->saveProtoForPlugin();
    }
}

void CannedMessagePlugin::handleSetCannedMessagePluginPart3(const char *from_msg)
{
    int changed = 0;

    if (*from_msg)
    {
        changed |= strcmp(cannedMessagePluginConfig.messagesPart3, from_msg);
        strcpy(cannedMessagePluginConfig.messagesPart3, from_msg);
    }

    if (changed)
    {
        this->saveProtoForPlugin();
    }
}

void CannedMessagePlugin::handleSetCannedMessagePluginPart4(const char *from_msg)
{
    int changed = 0;

    if (*from_msg)
    {
        changed |= strcmp(cannedMessagePluginConfig.messagesPart4, from_msg);
        strcpy(cannedMessagePluginConfig.messagesPart4, from_msg);
    }

    if (changed)
    {
        this->saveProtoForPlugin();
    }
}
