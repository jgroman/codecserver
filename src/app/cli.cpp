#include "config.h"
#include "cli.hpp"
#include "handshake.pb.h"
#include "request.pb.h"
#include "response.pb.h"
#include "framing.pb.h"
#include "data.pb.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <unistd.h>
#include <iostream>
#include <stdio.h>
#include <google/protobuf/any.pb.h>

using namespace CodecServer;
using namespace CodecServer::proto;

int Cli::main(int argc, char** argv) {
    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    const char* socket_path = "/tmp/codecserver.sock";
    strncpy(addr.sun_path, socket_path, strlen(socket_path));

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        std::cerr << "socket error\n";
        return 1;
    }

    if (connect(sock, (struct sockaddr*) &addr, sizeof(addr)) == -1) {
        std::cerr << "connection failure\n";
        return 1;
    }

    Connection* connection = new Connection(sock);
    google::protobuf::Any* message = connection->receiveMessage();
    if (!message->Is<Handshake>()) {
        std::cerr << "unexpected message\n";
        return 1;
    }

    Handshake handshake;
    message->UnpackTo(&handshake);

    std::cerr << "received handshake from " << handshake.servername() << "\n";

    if (handshake.serverversion() != VERSION) {
        std::cerr << "server version mismatch\n";
        return 1;
    }

    Request request;
    request.set_codec("ambe");
    request.clear_direction();
    request.mutable_direction()->Add(Request_Direction_DECODE);
    (*request.mutable_args())["index"] = "33";
    connection->sendMessage(&request);

    message = connection->receiveMessage();
    if (!message->Is<Response>()) {
        std::cerr << "response error\n";
        return 1;
    }

    Response response;
    message->UnpackTo(&response);

    if (response.result() != Response_Status_OK) {
        std::cerr << "server replied with error, message: " << response.message() << "\n";
        return 1;
    }

    if (!response.has_framing()) {
        std::cerr << "framing info is not available\n";
        return 1;
    }
    FramingHint framing = response.framing();

    std::cerr << "server response OK, start decoding!\n";

    char* in_buf = (char*) malloc(framing.channelbytes());
    fd_set read_fds;
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    int rc;
    int nfds = std::max(fileno(stdin), sock) + 1;

    while (run) {
        FD_ZERO(&read_fds);
        FD_SET(fileno(stdin), &read_fds);
        FD_SET(sock, &read_fds);

        rc = select(nfds, &read_fds, NULL, NULL, &tv);
        if (rc == -1) {
            std::cerr << "select() error\n";
            run = false;
        } else if (rc) {
            if (FD_ISSET(fileno(stdin), &read_fds)) {
                size_t size = fread(in_buf, sizeof(char), framing.channelbytes(), stdin);
                if (size <= 0) {
                    run = false;
                    break;
                }
                connection->sendChannelData(in_buf, size);
            }
            if (FD_ISSET(sock, &read_fds)) {
                google::protobuf::Any* message = connection->receiveMessage();
                if (message->Is<SpeechData>()) {
                    SpeechData* data = new SpeechData();
                    message->UnpackTo(data);
                    std::string output = data->data();
                    fwrite(output.data(), sizeof(char), output.length(), stdout);
                    fflush(stdout);
                    delete data;
                } else {
                    std::cerr << "received unexpected message type\n";
                }
            }
        } else {
            // no data, just timeout.
        }

    }

    free(in_buf);

    ::close(sock);

    return 0;
}