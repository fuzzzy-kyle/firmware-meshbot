#include "BotModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "configuration.h"
#include "main.h"
#include <Arduino.h>

BotModule *botModule;

BotModule::BotModule()
    : SinglePortModule("bot", meshtastic_PortNum_TEXT_MESSAGE_APP), concurrency::OSThread("BotModule")
{
    // Initialize scheduled messages
    for (int i = 0; i < BOT_MAX_SCHEDULED_MESSAGES; i++) {
        scheduledMessages[i].enabled = false;
        scheduledMessages[i].intervalSeconds = 0;
        scheduledMessages[i].nextSendTime = 0;
        scheduledMessages[i].targetNode = NODENUM_BROADCAST;
        scheduledMessages[i].channel = 0;
        memset(scheduledMessages[i].message, 0, BOT_MAX_RESPONSE_LENGTH);
    }

    // Load config from moduleConfig
    if (moduleConfig.has_bot) {
        botEnabled = moduleConfig.bot.enabled;
        rateLimitSeconds = moduleConfig.bot.rate_limit_seconds;
        respondToAll = moduleConfig.bot.respond_to_all;
        LOG_INFO("BotModule config loaded: enabled=%d, rate_limit=%d, respond_to_all=%d",
                 botEnabled, rateLimitSeconds, respondToAll);
    } else {
        // Set defaults
        botEnabled = true; // Enabled by default for testing
        rateLimitSeconds = 10;
        respondToAll = true;
        LOG_INFO("BotModule using defaults (enabled)");
    }

    if (botEnabled) {
        LOG_INFO("BotModule initialized and enabled");
    } else {
        LOG_INFO("BotModule initialized but disabled");
    }
}

ProcessMessage BotModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Only handle text messages
    if (mp.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP)
        return ProcessMessage::CONTINUE;

    // Check if bot is enabled
    if (!botEnabled) {
        return ProcessMessage::CONTINUE;
    }

    // Extract message text first to check if it's a config command
    auto &p = mp.decoded;
    if (p.payload.size == 0 || p.payload.size > meshtastic_Constants_DATA_PAYLOAD_LEN) {
        return ProcessMessage::CONTINUE;
    }

    char text[meshtastic_Constants_DATA_PAYLOAD_LEN + 1];
    memcpy(text, p.payload.bytes, p.payload.size);
    text[p.payload.size] = '\0';

    // Don't respond to our own messages UNLESS it's a config command
    auto ourNode = nodeDB->getNodeNum();
    bool isConfigCommand = (strncmp(text, "!botconfig", 10) == 0);
    if (mp.from == ourNode && !isConfigCommand) {
        return ProcessMessage::CONTINUE;
    }

    // Check if node is allowed
    if (!isAllowed(mp.from)) {
        LOG_DEBUG("BotModule: Ignoring message from blocked node 0x%x", mp.from);
        return ProcessMessage::CONTINUE;
    }

    // Check rate limiting
    if (isRateLimited(mp.from)) {
        LOG_DEBUG("BotModule: Rate limited for node 0x%x", mp.from);
        return ProcessMessage::CONTINUE;
    }

    LOG_INFO("BotModule: Received message from=0x%x, channel=%d, msg=%s", mp.from, mp.channel, text);

    // Handle command and send response if applicable
    if (handleCommand(text, mp.from, mp.channel)) {
        // Update rate limit
        lastMessageTime[mp.from] = millis();
    }

    // Let other modules process this message too
    return ProcessMessage::CONTINUE;
}

bool BotModule::handleCommand(const char *text, uint32_t fromNode, uint8_t channel)
{
    // Check if message starts with command prefix
    if (text[0] != '!') {
        return false;
    }

    const char *response = nullptr;

    // Check for config commands first (requires admin)
    if (strncmp(text, "!botconfig", 10) == 0) {
        return handleConfigCommand(text, fromNode, channel);
    }

    // Built-in commands
    if (strcmp(text, "!ping") == 0) {
        response = handlePing();
    } else if (strcmp(text, "!help") == 0 || strcmp(text, "!commands") == 0) {
        response = handleHelp();
    } else if (strcmp(text, "!info") == 0) {
        response = handleInfo();
    } else if (strcmp(text, "!uptime") == 0) {
        response = handleUptime();
    } else if (strcmp(text, "!stats") == 0) {
        response = handleStats();
    } else {
        // Check custom commands
        for (auto &cmd : customCommands) {
            if (cmd.enabled && strcmp(text, cmd.command) == 0) {
                response = cmd.response;
                break;
            }
        }
    }

    // Send response if we have one
    if (response != nullptr) {
        sendBotMessage(response, fromNode, channel);
        return true;
    }

    return false;
}

const char *BotModule::handlePing()
{
    return "Pong! 🤖";
}

const char *BotModule::handleHelp()
{
    // Use custom response if configured
    if (moduleConfig.has_bot && moduleConfig.bot.help_response[0] != '\0') {
        return moduleConfig.bot.help_response;
    }

    // Default help response
    static char helpText[BOT_MAX_RESPONSE_LENGTH];
    snprintf(helpText, sizeof(helpText),
             "Bot Commands:\n"
             "!ping - Test bot\n"
             "!info - Node info\n"
             "!uptime - System uptime\n"
             "!stats - Mesh stats\n"
             "!help - This message");
    return helpText;
}

const char *BotModule::handleInfo()
{
    // Use custom response if configured
    if (moduleConfig.has_bot && moduleConfig.bot.info_response[0] != '\0') {
        return moduleConfig.bot.info_response;
    }

    // Default info response
    static char infoText[BOT_MAX_RESPONSE_LENGTH];
    auto ourNode = nodeDB->getMeshNode(nodeDB->getNodeNum());

    if (ourNode && ourNode->has_user) {
        snprintf(infoText, sizeof(infoText), "Node: %s\nID: %08x\nVersion: %s", ourNode->user.long_name,
                 nodeDB->getNodeNum(), optstr(APP_VERSION));
    } else {
        snprintf(infoText, sizeof(infoText), "Node ID: %08x\nVersion: %s", nodeDB->getNodeNum(), optstr(APP_VERSION));
    }

    return infoText;
}

const char *BotModule::handleUptime()
{
    static char uptimeText[BOT_MAX_RESPONSE_LENGTH];
    uint32_t uptime = millis() / 1000;
    uint32_t days = uptime / 86400;
    uint32_t hours = (uptime % 86400) / 3600;
    uint32_t minutes = (uptime % 3600) / 60;
    uint32_t seconds = uptime % 60;

    if (days > 0) {
        snprintf(uptimeText, sizeof(uptimeText), "Uptime: %ud %uh %um %us", days, hours, minutes, seconds);
    } else if (hours > 0) {
        snprintf(uptimeText, sizeof(uptimeText), "Uptime: %uh %um %us", hours, minutes, seconds);
    } else {
        snprintf(uptimeText, sizeof(uptimeText), "Uptime: %um %us", minutes, seconds);
    }

    return uptimeText;
}

const char *BotModule::handleStats()
{
    static char statsText[BOT_MAX_RESPONSE_LENGTH];

    // Get node count from database
    uint32_t nodeCount = 0;
    for (int i = 0; i < nodeDB->getNumMeshNodes(); i++) {
        auto node = nodeDB->getMeshNode(i);
        if (node && node->has_user) {
            nodeCount++;
        }
    }

    snprintf(statsText, sizeof(statsText), "Nodes in DB: %u\nFree heap: %u bytes", nodeCount,
             ESP.getFreeHeap()); // ESP32-specific

    return statsText;
}

bool BotModule::handleConfigCommand(const char *text, uint32_t fromNode, uint8_t channel)
{
    static char responseText[BOT_MAX_RESPONSE_LENGTH];

    // Only allow local commands (from == 0 means sent locally via phone/serial)
    // This prevents other mesh users from changing your bot config
    if (fromNode != 0) {
        snprintf(responseText, sizeof(responseText), "Access denied: Config commands must be sent locally");
        sendBotMessage(responseText, fromNode, channel);
        LOG_WARN("BotModule: Unauthorized config attempt from 0x%x", fromNode);
        return true;
    }

    // Parse command after "!botconfig "
    const char *cmd = text + 10; // Skip "!botconfig"

    // Skip whitespace
    while (*cmd == ' ') cmd++;

    // !botconfig enable
    if (strncmp(cmd, "enable", 6) == 0) {
        botEnabled = true;
        snprintf(responseText, sizeof(responseText), "Bot enabled");
        sendBotMessage(responseText, fromNode, channel);
        return true;
    }

    // !botconfig disable
    if (strncmp(cmd, "disable", 7) == 0) {
        botEnabled = false;
        snprintf(responseText, sizeof(responseText), "Bot disabled");
        sendBotMessage(responseText, fromNode, channel);
        return true;
    }

    // !botconfig status
    if (strncmp(cmd, "status", 6) == 0) {
        snprintf(responseText, sizeof(responseText),
                 "Bot: %s\nRate limit: %us\nRespond to all: %s\nCustom cmds: %u\nScheduled msgs: %u",
                 botEnabled ? "enabled" : "disabled",
                 rateLimitSeconds,
                 respondToAll ? "yes" : "no",
                 (uint32_t)customCommands.size(),
                 scheduledMessageCount);
        sendBotMessage(responseText, fromNode, channel);
        return true;
    }

    // !botconfig ratelimit <seconds>
    if (strncmp(cmd, "ratelimit ", 10) == 0) {
        uint32_t seconds = atoi(cmd + 10);
        if (seconds <= 3600) {
            rateLimitSeconds = seconds;
            snprintf(responseText, sizeof(responseText), "Rate limit set to %u seconds", seconds);
        } else {
            snprintf(responseText, sizeof(responseText), "Invalid rate limit (0-3600)");
        }
        sendBotMessage(responseText, fromNode, channel);
        return true;
    }

    // !botconfig respondall <on|off>
    if (strncmp(cmd, "respondall ", 11) == 0) {
        const char *value = cmd + 11;
        if (strcmp(value, "on") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "true") == 0) {
            respondToAll = true;
            snprintf(responseText, sizeof(responseText), "Respond to all: enabled");
        } else if (strcmp(value, "off") == 0 || strcmp(value, "no") == 0 || strcmp(value, "false") == 0) {
            respondToAll = false;
            snprintf(responseText, sizeof(responseText), "Respond to all: disabled");
        } else {
            snprintf(responseText, sizeof(responseText), "Usage: !botconfig respondall <on|off>");
        }
        sendBotMessage(responseText, fromNode, channel);
        return true;
    }

    // !botconfig schedule <interval_sec> <message>
    if (strncmp(cmd, "schedule ", 9) == 0) {
        const char *params = cmd + 9;
        uint32_t interval = atoi(params);

        // Find the space after interval to get message
        const char *messageStart = strchr(params, ' ');
        if (messageStart) {
            messageStart++; // Skip the space

            if (addScheduledMessage(messageStart, interval, NODENUM_BROADCAST, channel)) {
                snprintf(responseText, sizeof(responseText), "Scheduled message added (every %us)", interval);
            } else {
                snprintf(responseText, sizeof(responseText), "Failed to add (limit reached or interval < 60s)");
            }
        } else {
            snprintf(responseText, sizeof(responseText), "Usage: !botconfig schedule <sec> <message>");
        }
        sendBotMessage(responseText, fromNode, channel);
        return true;
    }

    // !botconfig unschedule <index>
    if (strncmp(cmd, "unschedule ", 11) == 0) {
        uint8_t index = atoi(cmd + 11);
        if (removeScheduledMessage(index)) {
            snprintf(responseText, sizeof(responseText), "Scheduled message %d removed", index);
        } else {
            snprintf(responseText, sizeof(responseText), "Invalid index %d", index);
        }
        sendBotMessage(responseText, fromNode, channel);
        return true;
    }

    // !botconfig listschedule
    if (strncmp(cmd, "listschedule", 12) == 0) {
        if (scheduledMessageCount == 0) {
            snprintf(responseText, sizeof(responseText), "No scheduled messages");
        } else {
            snprintf(responseText, sizeof(responseText), "Scheduled messages: %d\nUse !botconfig status for details", scheduledMessageCount);
        }
        sendBotMessage(responseText, fromNode, channel);
        return true;
    }

    // !botconfig help
    if (strncmp(cmd, "help", 4) == 0 || strlen(cmd) == 0) {
        snprintf(responseText, sizeof(responseText),
                 "Bot Config:\n"
                 "!botconfig status\n"
                 "!botconfig enable/disable\n"
                 "!botconfig ratelimit <sec>\n"
                 "!botconfig respondall <on|off>\n"
                 "!botconfig schedule <sec> <msg>\n"
                 "!botconfig unschedule <idx>\n"
                 "!botconfig listschedule");
        sendBotMessage(responseText, fromNode, channel);
        return true;
    }

    // Unknown config command
    snprintf(responseText, sizeof(responseText), "Unknown config command. Try !botconfig help");
    sendBotMessage(responseText, fromNode, channel);
    return true;
}

void BotModule::sendBotMessage(const char *message, uint32_t toNode, uint8_t channel)
{
    auto packet = createResponse(toNode, message, channel);
    if (packet) {
        service->sendToMesh(packet, RX_SRC_LOCAL, true);
        LOG_INFO("BotModule: Sent response to 0x%x on channel %d: %s", toNode, channel, message);
    }
}

meshtastic_MeshPacket *BotModule::createResponse(uint32_t toNode, const char *message, uint8_t channel)
{
    auto packet = allocDataPacket();
    if (!packet) {
        LOG_ERROR("BotModule: Failed to allocate packet");
        return nullptr;
    }

    packet->to = toNode;
    packet->channel = channel;
    packet->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;

    size_t len = strlen(message);
    if (len > meshtastic_Constants_DATA_PAYLOAD_LEN) {
        len = meshtastic_Constants_DATA_PAYLOAD_LEN;
    }

    packet->decoded.payload.size = len;
    memcpy(packet->decoded.payload.bytes, message, len);

    return packet;
}

bool BotModule::isRateLimited(uint32_t fromNode)
{
    if (rateLimitSeconds == 0) {
        return false;
    }

    auto it = lastMessageTime.find(fromNode);
    if (it == lastMessageTime.end()) {
        return false;
    }

    uint32_t elapsed = (millis() - it->second) / 1000;
    return elapsed < rateLimitSeconds;
}

bool BotModule::isAllowed(uint32_t fromNode)
{
    // Check block list first
    for (auto blockedNode : blockedNodes) {
        if (blockedNode == fromNode) {
            return false;
        }
    }

    // If respondToAll is true and node not blocked, allow
    if (respondToAll) {
        return true;
    }

    // Check allow list
    for (auto allowedNode : allowedNodes) {
        if (allowedNode == fromNode) {
            return true;
        }
    }

    return false;
}

int32_t BotModule::runOnce()
{
    if (botEnabled) {
        processScheduledMessages();
    }

    // Check every second
    return 1000;
}

void BotModule::processScheduledMessages()
{
    uint32_t now = millis();

    for (int i = 0; i < scheduledMessageCount && i < BOT_MAX_SCHEDULED_MESSAGES; i++) {
        auto &msg = scheduledMessages[i];

        if (!msg.enabled) {
            continue;
        }

        // Check if it's time to send
        if (now >= msg.nextSendTime) {
            LOG_INFO("BotModule: Sending scheduled message: %s", msg.message);
            sendBotMessage(msg.message, msg.targetNode, msg.channel);

            // Schedule next send
            msg.nextSendTime = now + (msg.intervalSeconds * 1000);
        }
    }
}

// Public API methods

void BotModule::setEnabled(bool enabled)
{
    botEnabled = enabled;
    LOG_INFO("BotModule: %s", enabled ? "Enabled" : "Disabled");
}

bool BotModule::addScheduledMessage(const char *message, uint32_t intervalSeconds, uint32_t targetNode, uint8_t channel)
{
    if (scheduledMessageCount >= BOT_MAX_SCHEDULED_MESSAGES) {
        LOG_ERROR("BotModule: Cannot add scheduled message, limit reached");
        return false;
    }

    if (intervalSeconds < 60) {
        LOG_ERROR("BotModule: Scheduled message interval must be at least 60 seconds");
        return false;
    }

    auto &msg = scheduledMessages[scheduledMessageCount];
    strncpy(msg.message, message, BOT_MAX_RESPONSE_LENGTH - 1);
    msg.message[BOT_MAX_RESPONSE_LENGTH - 1] = '\0';
    msg.intervalSeconds = intervalSeconds;
    msg.targetNode = targetNode;
    msg.channel = channel;
    msg.enabled = true;
    msg.nextSendTime = millis() + (intervalSeconds * 1000);

    scheduledMessageCount++;
    LOG_INFO("BotModule: Added scheduled message (count=%d)", scheduledMessageCount);

    return true;
}

bool BotModule::removeScheduledMessage(uint8_t index)
{
    if (index >= scheduledMessageCount) {
        return false;
    }

    // Shift messages down
    for (int i = index; i < scheduledMessageCount - 1; i++) {
        scheduledMessages[i] = scheduledMessages[i + 1];
    }

    scheduledMessageCount--;
    scheduledMessages[scheduledMessageCount].enabled = false;

    LOG_INFO("BotModule: Removed scheduled message (count=%d)", scheduledMessageCount);
    return true;
}

bool BotModule::addCustomCommand(const char *command, const char *response)
{
    if (customCommands.size() >= 10) { // Limit to 10 custom commands
        LOG_ERROR("BotModule: Cannot add custom command, limit reached");
        return false;
    }

    CustomCommand cmd;
    strncpy(cmd.command, command, BOT_MAX_COMMAND_LENGTH - 1);
    cmd.command[BOT_MAX_COMMAND_LENGTH - 1] = '\0';
    strncpy(cmd.response, response, BOT_MAX_RESPONSE_LENGTH - 1);
    cmd.response[BOT_MAX_RESPONSE_LENGTH - 1] = '\0';
    cmd.enabled = true;

    customCommands.push_back(cmd);
    LOG_INFO("BotModule: Added custom command: %s", command);

    return true;
}

bool BotModule::removeCustomCommand(const char *command)
{
    for (auto it = customCommands.begin(); it != customCommands.end(); ++it) {
        if (strcmp(it->command, command) == 0) {
            customCommands.erase(it);
            LOG_INFO("BotModule: Removed custom command: %s", command);
            return true;
        }
    }

    return false;
}

void BotModule::setRateLimit(uint32_t seconds)
{
    rateLimitSeconds = seconds;
    LOG_INFO("BotModule: Rate limit set to %u seconds", seconds);
}

void BotModule::addAllowedNode(uint32_t nodeId)
{
    allowedNodes.push_back(nodeId);
    LOG_INFO("BotModule: Added allowed node 0x%x", nodeId);
}

void BotModule::addBlockedNode(uint32_t nodeId)
{
    blockedNodes.push_back(nodeId);
    LOG_INFO("BotModule: Added blocked node 0x%x", nodeId);
}
