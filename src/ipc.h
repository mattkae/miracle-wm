#ifndef MIRACLEWM_IPC_H
#define MIRACLEWM_IPC_H

#include "workspace_manager.h"
#include "workspace_observer.h"
#include <miral/runner.h>
#include <mir/fd.h>
#include <vector>

struct sockaddr_un;

namespace miracle
{

/// This it taken directly from SWAY
enum IpcCommandType {
    // i3 command types - see i3's I3_REPLY_TYPE constants
    IPC_COMMAND = 0,
    IPC_GET_WORKSPACES = 1,
    IPC_SUBSCRIBE = 2,
    IPC_GET_OUTPUTS = 3,
    IPC_GET_TREE = 4,
    IPC_GET_MARKS = 5,
    IPC_GET_BAR_CONFIG = 6,
    IPC_GET_VERSION = 7,
    IPC_GET_BINDING_MODES = 8,
    IPC_GET_CONFIG = 9,
    IPC_SEND_TICK = 10,
    IPC_SYNC = 11,
    IPC_GET_BINDING_STATE = 12,

    // sway-specific command types
    IPC_GET_INPUTS = 100,
    IPC_GET_SEATS = 101,

    // Events sent from sway to clients. Events have the highest bits set.
    IPC_EVENT_WORKSPACE = ((1<<31) | 0),
    IPC_EVENT_OUTPUT = ((1<<31) | 1),
    IPC_EVENT_MODE = ((1<<31) | 2),
    IPC_EVENT_WINDOW = ((1<<31) | 3),
    IPC_EVENT_BARCONFIG_UPDATE = ((1<<31) | 4),
    IPC_EVENT_BINDING = ((1<<31) | 5),
    IPC_EVENT_SHUTDOWN = ((1<<31) | 6),
    IPC_EVENT_TICK = ((1<<31) | 7),

    // sway-specific event types
    IPC_EVENT_BAR_STATE_UPDATE = ((1<<31) | 20),
    IPC_EVENT_INPUT = ((1<<31) | 21),
};

/// Inter process communication for compositor clients (e.g. waybar).
/// This class will implement I3's interface: https://i3wm.org/docs/ipc.html
/// plus some of the sway-specific items.
/// It may be extended in the future.
class Ipc : public WorkspaceObserver
{
public:
    Ipc(miral::MirRunner& runner, WorkspaceManager&);

    void on_created(std::shared_ptr<OutputContent> const& info, int key) override;
    void on_removed(std::shared_ptr<OutputContent> const& info, int key) override;
    void on_focused(std::shared_ptr<OutputContent> const& previous, int, std::shared_ptr<OutputContent> const& current, int) override;
    void disconnect_all();
private:
    struct IpcClient
    {
        mir::Fd client_fd;
        std::unique_ptr<miral::FdHandle> handle;
        uint32_t pending_read_length = 0;
        IpcCommandType pending_type;
        std::vector<char> buffer;
        int write_buffer_len = 0;
        int subscribed_events = 0;
    };

    WorkspaceManager& workspace_manager;
    mir::Fd ipc_socket;
    std::unique_ptr<miral::FdHandle> socket_handle;
    sockaddr_un* ipc_sockaddr = nullptr;
    std::vector<IpcClient> clients;

    void disconnect(IpcClient& client);
    IpcClient& get_client(int fd);
    void handle_command(IpcClient& client, uint32_t payload_length, IpcCommandType payload_type);
    void send_reply(IpcClient& client, IpcCommandType command_type, std::string const& payload);
    void handle_writeable(IpcClient& client);
};
}


#endif //MIRACLEWM_IPC_H
