/**
 * @brief IPC server for controlling Waifuland from external programs.
 *
 * Listens on a Unix domain socket and accepts newline-delimited JSON commands.
 * Socket path: $XDG_RUNTIME_DIR/waifuland.sock (fallback: /tmp/waifuland.sock)
 */

#pragma once

#include <string>
#include <vector>

class LAppIPC
{
public:
    static LAppIPC* GetInstance();
    static void ReleaseInstance();

    /**
     * @brief Initialize the IPC socket server.
     * @return true on success
     */
    bool Initialize();

    /**
     * @brief Poll for incoming connections and process commands.
     *        Call this once per frame from the main loop.
     */
    void Poll();

    /**
     * @brief Clean up the socket.
     */
    void Release();

    /**
     * @brief Get the socket path being used.
     */
    const std::string& GetSocketPath() const { return _socketPath; }

    /**
     * @brief Set the mouth Y value from external source (for lipsync).
     *        Read by LAppModel during Update().
     */
    static float GetExternalMouthY() { return _externalMouthY; }
    static bool HasExternalMouthY() { return _hasExternalMouthY; }
    static void ClearExternalMouthY() { _hasExternalMouthY = false; }

    /**
     * @brief External look-at override.
     */
    static bool HasExternalLook() { return _hasExternalLook; }
    static float GetExternalLookX() { return _externalLookX; }
    static float GetExternalLookY() { return _externalLookY; }

private:
    LAppIPC();
    ~LAppIPC();

    std::string ProcessCommand(const std::string& json);
    void HandleClient(int clientFd);

    int _serverFd;
    std::string _socketPath;
    std::vector<int> _clients;

    // Partial read buffers per client fd
    struct ClientBuffer {
        int fd;
        std::string buf;
    };
    std::vector<ClientBuffer> _clientBuffers;

    std::string& GetClientBuffer(int fd);
    void RemoveClientBuffer(int fd);

    static float _externalMouthY;
    static bool _hasExternalMouthY;
    static float _externalLookX;
    static float _externalLookY;
    static bool _hasExternalLook;
};
