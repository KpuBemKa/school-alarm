#include "http_server.hpp"

#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_random.h"
#include <fcntl.h>
#include <string.h>
// #include "esp_vfs.h"
#include "cJSON.h"

#define ESP_VFS_PATH_MAX 255

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE (10240)

#define CHECK_FILE_EXTENSION(filename, ext)                                    \
  (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

namespace server {

constexpr const char* TAG = "HTTP SERVER";

/* Set HTTP response content type according to file extension */
esp_err_t
HttpServer::set_content_type_from_file(httpd_req_t* req, const char* filepath)
{
  const char* type = "text/plain";
  if (CHECK_FILE_EXTENSION(filepath, ".html")) {
    type = "text/html";
  } else if (CHECK_FILE_EXTENSION(filepath, ".js")) {
    type = "application/javascript";
  } else if (CHECK_FILE_EXTENSION(filepath, ".css")) {
    type = "text/css";
  } else if (CHECK_FILE_EXTENSION(filepath, ".png")) {
    type = "image/png";
  } else if (CHECK_FILE_EXTENSION(filepath, ".ico")) {
    type = "image/x-icon";
  } else if (CHECK_FILE_EXTENSION(filepath, ".svg")) {
    type = "text/xml";
  }
  return httpd_resp_set_type(req, type);
}

// /* Send HTTP response with the contents of the requested file */
// static esp_err_t
// HttpServer::rest_common_get_handler(httpd_req_t* req)
// {
//   char filepath[FILE_PATH_MAX];

//   RestServerContext* rest_context = (RestServerContext*)req->user_ctx;
//   strlcpy(filepath, rest_context->base_path, sizeof(filepath));
//   if (req->uri[strlen(req->uri) - 1] == '/') {
//     strlcat(filepath, "/index.html", sizeof(filepath));
//   } else {
//     strlcat(filepath, req->uri, sizeof(filepath));
//   }
//   int fd = open(filepath, O_RDONLY, 0);
//   if (fd == -1) {
//     ESP_LOGE(REST_TAG, "Failed to open file : %s", filepath);
//     /* Respond with 500 Internal Server Error */
//     httpd_resp_send_err(
//       req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
//     return ESP_FAIL;
//   }

//   set_content_type_from_file(req, filepath);

//   char* chunk = rest_context->scratch;
//   ssize_t read_bytes;
//   do {
//     /* Read file in chunks into the scratch buffer */
//     read_bytes = read(fd, chunk, SCRATCH_BUFSIZE);
//     if (read_bytes == -1) {
//       ESP_LOGE(REST_TAG, "Failed to read file : %s", filepath);
//     } else if (read_bytes > 0) {
//       /* Send the buffer contents as HTTP response chunk */
//       if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
//         close(fd);
//         ESP_LOGE(REST_TAG, "File sending failed!");
//         /* Abort sending file */
//         httpd_resp_sendstr_chunk(req, NULL);
//         /* Respond with 500 Internal Server Error */
//         httpd_resp_send_err(
//           req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
//         return ESP_FAIL;
//       }
//     }
//   } while (read_bytes > 0);
//   /* Close file after sending complete */
//   close(fd);
//   ESP_LOGI(REST_TAG, "File sending complete");
//   /* Respond with an empty chunk to signal HTTP response completion */
//   httpd_resp_send_chunk(req, NULL, 0);
//   return ESP_OK;
// }

// /* Simple handler for light brightness control */
// static esp_err_t
// HttpServer::light_brightness_post_handler(httpd_req_t* req)
// {
//   int total_len = req->content_len;
//   int cur_len = 0;
//   char* buf = ((RestServerContext*)(req->user_ctx))->scratch;
//   int received = 0;
//   if (total_len >= SCRATCH_BUFSIZE) {
//     /* Respond with 500 Internal Server Error */
//     httpd_resp_send_err(
//       req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
//     return ESP_FAIL;
//   }
//   while (cur_len < total_len) {
//     received = httpd_req_recv(req, buf + cur_len, total_len);
//     if (received <= 0) {
//       /* Respond with 500 Internal Server Error */
//       httpd_resp_send_err(
//         req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control
//         value");
//       return ESP_FAIL;
//     }
//     cur_len += received;
//   }
//   buf[total_len] = '\0';

//   cJSON* root = cJSON_Parse(buf);
//   int red = cJSON_GetObjectItem(root, "red")->valueint;
//   int green = cJSON_GetObjectItem(root, "green")->valueint;
//   int blue = cJSON_GetObjectItem(root, "blue")->valueint;
//   ESP_LOGI(REST_TAG,
//            "Light control: red = %d, green = %d, blue = %d",
//            red,
//            green,
//            blue);
//   cJSON_Delete(root);
//   httpd_resp_sendstr(req, "Post control value successfully");
//   return ESP_OK;
// }

// /* Simple handler for getting system handler */
// static esp_err_t
// HttpServer::system_info_get_handler(httpd_req_t* req)
// {
//   httpd_resp_set_type(req, "application/json");
//   cJSON* root = cJSON_CreateObject();
//   esp_chip_info_t chip_info;
//   esp_chip_info(&chip_info);
//   cJSON_AddStringToObject(root, "version", IDF_VER);
//   cJSON_AddNumberToObject(root, "cores", chip_info.cores);
//   const char* sys_info = cJSON_Print(root);
//   httpd_resp_sendstr(req, sys_info);
//   free((void*)sys_info);
//   cJSON_Delete(root);
//   return ESP_OK;
// }

// /* Simple handler for getting temperature data */
// static esp_err_t
// HttpServer::temperature_data_get_handler(httpd_req_t* req)
// {
//   httpd_resp_set_type(req, "application/json");
//   cJSON* root = cJSON_CreateObject();
//   cJSON_AddNumberToObject(root, "raw", esp_random() % 20);
//   const char* sys_info = cJSON_Print(root);
//   httpd_resp_sendstr(req, sys_info);
//   free((void*)sys_info);
//   cJSON_Delete(root);
//   return ESP_OK;
// }

esp_err_t
HttpServer::StartServer(std::string_view base_path)
{
  mRestContext.base_path = base_path;
  // strlcpy(rest_context->base_path, base_path,
  // sizeof(rest_context->base_path));

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.uri_match_fn = httpd_uri_match_wildcard;

  ESP_LOGI(TAG, "Starting HTTP Server...");

  esp_err_t result = httpd_start(&mServerHandle, &config);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start() failed: %s", esp_err_to_name(result));
    return result;
  }

  // /* URI handler for fetching system info */
  // httpd_uri_t system_info_get_uri = { .uri = "/api/v1/system/info",
  //                                     .method = HTTP_GET,
  //                                     .handler = system_info_get_handler,
  //                                     .user_ctx = &mRestContext };
  // httpd_register_uri_handler(mServerHandle, &system_info_get_uri);

  // /* URI handler for fetching temperature data */
  // httpd_uri_t temperature_data_get_uri = { .uri = "/api/v1/temp/raw",
  //                                          .method = HTTP_GET,
  //                                          .handler =
  //                                            temperature_data_get_handler,
  //                                          .user_ctx = &mRestContext };
  // httpd_register_uri_handler(mServerHandle, &temperature_data_get_uri);

  // /* URI handler for light brightness control */
  // httpd_uri_t light_brightness_post_uri = { .uri =
  // "/api/v1/light/brightness",
  //                                           .method = HTTP_POST,
  //                                           .handler =
  //                                             light_brightness_post_handler,
  //                                           .user_ctx = &mRestContext };
  // httpd_register_uri_handler(mServerHandle, &light_brightness_post_uri);

  // /* URI handler for getting web server files */
  // httpd_uri_t common_get_uri = { .uri = "/*",
  //                                .method = HTTP_GET,
  //                                .handler = rest_common_get_handler,
  //                                .user_ctx = &mRestContext };
  // httpd_register_uri_handler(mServerHandle, &common_get_uri);

  return ESP_OK;
}

}