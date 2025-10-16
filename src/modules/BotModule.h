#pragma once
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include <map>
#include <vector>

// Maximum number of scheduled messages
#define BOT_MAX_SCHEDULED_MESSAGES 20
// Maximum command response length
#define BOT_MAX_RESPONSE_LENGTH 200
// Maximum custom command name length
#define BOT_MAX_COMMAND_LENGTH 32

/**
 * Scheduled message structure
 */
struct ScheduledMessage {
    uint32_t intervalSeconds;  // Interval between sends
    uint32_t nextSendTime;     // Next time to send (millis)
    char message[BOT_MAX_RESPONSE_LENGTH];
    uint32_t targetNode;       // 0 for broadcast, specific node ID otherwise
    uint8_t channel;           // Channel to send on
    bool enabled;              // Whether this scheduled message is active
};

/**
 * Custom command structure
 */
struct CustomCommand {
    char command[BOT_MAX_COMMAND_LENGTH];
    char response[BOT_MAX_RESPONSE_LENGTH];
    bool enabled;
};

/**
 * BotModule - Automated chatbot functionality
 *
 * Features:
 * - Command responses (e.g., !ping -> pong)
 * - Scheduled messages
 * - Rate limiting per user
 * - Configurable enable/disable
 * - Allow/block lists
 */
class BotModule : public SinglePortModule, private concurrency::OSThread
{
  private:
    // Rate limiting: track last message time per user
    std::map<uint32_t, uint32_t> lastMessageTime;

    // Scheduled messages array
    ScheduledMessage scheduledMessages[BOT_MAX_SCHEDULED_MESSAGES];
    uint8_t scheduledMessageCount = 0;

    // Custom commands
    std::vector<CustomCommand> customCommands;

    // Configuration (these will later come from protobuf config)
    bool botEnabled = true;
    uint32_t rateLimitSeconds = 10;  // Minimum seconds between responses to same user
    bool respondToAll = true;        // Respond to all users or only allowed list
    std::vector<uint32_t> allowedNodes;
    std::vector<uint32_t> blockedNodes;

    // Helper methods
    bool isRateLimited(uint32_t fromNode);
    bool isAllowed(uint32_t fromNode);
    meshtastic_MeshPacket *createResponse(uint32_t toNode, const char *message, uint8_t channel = 0);
    void sendBotMessage(const char *message, uint32_t toNode = NODENUM_BROADCAST, uint8_t channel = 0);
    bool handleCommand(const char *text, uint32_t fromNode, uint8_t channel);
    bool handleConfigCommand(const char *text, uint32_t fromNode, uint8_t channel);
    void processScheduledMessages();

    // Built-in command handlers
    const char *handlePing();
    const char *handleHelp();
    const char *handleInfo();
    const char *handleUptime();
    const char *handleStats();

  protected:
    /**
     * Called to handle a received message
     */
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

    /**
     * OSThread main loop for scheduled messages
     */
    virtual int32_t runOnce() override;

  public:
    BotModule();

    /**
     * Enable/disable the bot
     */
    void setEnabled(bool enabled);

    /**
     * Add a scheduled message
     */
    bool addScheduledMessage(const char *message, uint32_t intervalSeconds,
                            uint32_t targetNode = NODENUM_BROADCAST, uint8_t channel = 0);

    /**
     * Remove a scheduled message by index
     */
    bool removeScheduledMessage(uint8_t index);

    /**
     * Add a custom command
     */
    bool addCustomCommand(const char *command, const char *response);

    /**
     * Remove a custom command
     */
    bool removeCustomCommand(const char *command);

    /**
     * Set rate limit (seconds between responses to same user)
     */
    void setRateLimit(uint32_t seconds);

    /**
     * Add node to allow list
     */
    void addAllowedNode(uint32_t nodeId);

    /**
     * Add node to block list
     */
    void addBlockedNode(uint32_t nodeId);
};

extern BotModule *botModule;
