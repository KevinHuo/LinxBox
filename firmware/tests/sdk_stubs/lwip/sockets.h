#ifndef LINX_TEST_LWIP_SOCKETS_H
#define LINX_TEST_LWIP_SOCKETS_H
#define SHUT_RDWR 2
int test_wifi_socket_shutdown(int socket, int how);
int test_wifi_socket_close(int socket);
#define shutdown test_wifi_socket_shutdown
#define close test_wifi_socket_close
#endif
