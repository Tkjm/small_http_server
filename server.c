#define _GNU_SOURCE
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define MESSAGE_MAX_LENGTH 1200000
#define HEADER_MAX_LENGTH 20000
#define BODY_MAX_LENGTH 1200000 - 20000

void exit_with_response(const char response[]) {
  perror(response);
  exit(EXIT_FAILURE);
}

void exit_on_error(const int value, const char response[]) {
  if (value < 0) {
    exit_with_response(response);
  }
}

int default_socket() {
  int server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  exit_on_error(server_fd, "cannot create socket");
  return server_fd;
}

void bind_default(const int server_fd, const int port) {
  struct sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);
  int result = bind(server_fd, (struct sockaddr *)&address, sizeof(address));
  exit_on_error(result, "bind failed");
}

int open_default_server() {
  const int server = default_socket();
  const int PORT = 8080;
  exit_on_error(
      setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)),
      "setsockopt(SO_REUSEADDR) failed");
  bind_default(server, PORT);
  exit_on_error(listen(server, 10), "listen failed");
  return server;
}

typedef struct http_request {
  char *method;
  char *path;
  char *boundary;
  size_t content_length;
  char *body;
} http_request_t;

typedef struct http_response {
  int status_code;
  char reason_phrase[256];
  char content_type[256];
  size_t content_length;
  char location[256];
  char body[BODY_MAX_LENGTH];
} http_response_t;

void get_http_output(char *output, size_t *output_length,
                     const int output_max_length,
                     const http_response_t response) {
  int written_length = snprintf(output, output_max_length, "HTTP/1.1 %d %s\r\n",
                                response.status_code, response.reason_phrase);
  switch (response.status_code) {
  case 200:
  case 404:
    written_length +=
        snprintf(output + written_length, output_max_length - written_length,
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n\r\n",
                 response.content_type, response.content_length);
    memcpy(output + written_length, response.body, response.content_length);
    written_length += response.content_length;
    break;
  case 303:
    written_length +=
        snprintf(output + written_length, output_max_length - written_length,
                 "Location: %s\r\n", response.location);
    break;
  default:
    break;
  }
  *output_length = written_length;
}

void write_http_response(const int socket, const http_response_t response) {
  char output[MESSAGE_MAX_LENGTH];
  size_t output_length;
  get_http_output(output, &output_length, sizeof(output), response);
  exit_on_error(write(socket, output, output_length), "response failed");
}

bool prefix(const char *prefix, const char *str) {
  return strncmp(prefix, str, strlen(prefix)) == 0;
}

char *pos_after(const char *str, const char *needle) {
  char *temp = strstr(str, needle);
  if (temp) {
    return temp + strlen(needle);
  } else {
    return NULL;
  }
}

http_request_t parse_http_request(char *input) {
  http_request_t request = {};
  request.method = strtok_r(input, " \r\n", &input);
  request.path = strtok_r(NULL, " \r\n", &input);
  char *end_of_header = strstr(input, "\r\n\r\n");
  while (input && (!end_of_header || input <= end_of_header)) {
    const char *before_boundary =
        "Content-Type: multipart/form-data; boundary=";
    const char *before_length = "Content-Length: ";
    const char *header = strtok_r(NULL, "\r\n", &input);
    if (prefix(before_boundary, header)) {
      request.boundary = pos_after(header, before_boundary);
    } else if (prefix(before_length, header)) {
      request.content_length = atoi(pos_after(header, before_length));
    }
  }
  if (input) {
    request.body = pos_after(input, "\n\r\n");
  }
  return request;
}

http_request_t read_http_request(const int socket, char *buffer,
                                 const size_t buffer_length) {
  long read_length = read(socket, buffer, buffer_length - 1);
  buffer[read_length] = '\0';
  printf("Request length: %ld\n", read_length);
  http_request_t request = parse_http_request(buffer);
  if (request.content_length > 0) {
    while (request.content_length + request.body > buffer + read_length) {
      read_length +=
          read(socket, buffer + read_length, buffer_length - read_length - 1);
    }
  }
  return request;
}

void get_content_type(const char *pathname, char *content_type) {
  const char *extension = strrchr(pathname, '.');
  if (strcmp(extension, ".html") == 0 || strcmp(extension, ".htm") == 0) {
    strcpy(content_type, "text/html");
  } else if (strcmp(extension, ".css") == 0) {
    strcpy(content_type, "text/css");
  } else if (strcmp(extension, ".js") == 0) {
    strcpy(content_type, "text/javascript");
  } else if (strcmp(extension, ".ico") == 0) {
    strcpy(content_type, "image/x-icon");
  } else if (strcmp(extension, ".jpg") == 0 ||
             strcmp(extension, ".jpeg") == 0) {
    strcpy(content_type, "image/jpeg");
  } else {
    strcpy(content_type, "text/plain");
  }
}

void get_http_response(http_response_t *response, http_request_t request) {
  char *pathname = malloc((strlen(request.path) + 2) * sizeof(char));
  strcpy(pathname, ".");
  strcat(pathname, request.path);
  if (strcmp(pathname, "./") == 0) {
    strcpy(pathname, "./index.html");
  }
  if (access(pathname, R_OK) < 0 && strcmp(pathname, "./upload_bg") != 0) {
    strcpy(pathname, "./page_not_found.html");
    response->status_code = 404;
    strcpy(response->reason_phrase, "Not Found");
  }
  if (strcmp(pathname, "./upload_bg") == 0) {
    if (strcmp(request.method, "POST") == 0) {
      const char *image_start = pos_after(request.body, "image/jpeg\r\n\r\n");
      char end_pattern[1000];
      strcpy(end_pattern, "\r\n--");
      strcat(end_pattern, request.boundary);
      const char *image_end = memmem(
          image_start, request.content_length - (image_start - request.body),
          end_pattern, strlen(end_pattern));
      size_t image_size = image_end - image_start;
      const int image_file = open("bg.jpg", O_CREAT | O_WRONLY | O_TRUNC);
      exit_on_error(image_file, "image open failed");
      write(image_file, image_start, image_size);
      close(image_file);
    }
    response->status_code = 303;
    strcpy(response->reason_phrase, "See Other");
    strcpy(response->location, "/index.html");
  } else {
    const int file = open(pathname, O_RDONLY);
    exit_on_error(file, "open file failed");
    get_content_type(pathname, response->content_type);
    ssize_t read_bytes = read(file, response->body, sizeof(response->body));
    exit_on_error(read_bytes, "read file failed");
    printf("Read bytes: %zd\n", read_bytes);
    response->status_code = 200;
    strcpy(response->reason_phrase, "OK");
    response->content_length = read_bytes;
    close(file);
  }
  free(pathname);
}

void handle_connection(const int socket) {
  char buffer[MESSAGE_MAX_LENGTH];
  http_request_t request = read_http_request(socket, buffer, sizeof(buffer));
  printf("Method: %s\n"
         "Path: %s\n"
         "Boundary: %s\n"
         "Content length: %zu\n",
         request.method, request.path, request.boundary,
         request.content_length);
  http_response_t response;
  get_http_response(&response, request);
  write_http_response(socket, response);
}

int main() {
  const int server = open_default_server();
  while (true) {
    printf("+++++++ Waiting for new connection ++++++++\n");
    struct sockaddr_in client_address;
    socklen_t address_length = sizeof(client_address);
    int new_socket =
        accept(server, (struct sockaddr *)&client_address, &address_length);
    exit_on_error(new_socket, "accept failed");
    int pid = fork();
    if (!pid) {
      close(server);
      if (!fork()) {
        // this is the grandchild that keeps going
        handle_connection(new_socket);
      } else {
        // the first child process
        close(new_socket);
      }
      exit(EXIT_SUCCESS);
    }
    // this is the original process
    handle_connection(new_socket);
    close(new_socket);
    // wait for the first child to exit which it will immediately
    waitpid(pid, NULL, 0);
  }
  close(server);
  return 0;
}