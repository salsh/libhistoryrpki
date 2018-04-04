/*
 * This file is part of ROAFetchlib
 *
 * Author: Samir Al-Sheikh (Freie Universitaet, Berlin)
 *         s.al-sheikh@fu-berlin.de
 *
 * MIT License
 *
 * Copyright (c) 2017 The ROAFetchlib authors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "rtrlib/rtrlib.h"
#include "constants.h"
#include "debug.h"
#include "live_validation.h"
#include "rpki_config.h"
#include "wandio.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>

int live_validation_set_config(char* project, char* collector, rpki_cfg_t* cfg, char* ssh_options){

  // Check if the requested collector is a RTR-collector
  if(strstr(collector, "RTR") == NULL) {
    std_print("Error: Collector not allowed: %s (only RTR-Server)\n", collector);
    return -1;
  }

  // Build the info request URL
  config_broker_t *broker = &cfg->cfg_broker;
	char info_url[RPKI_BROKER_URL_LEN];
  snprintf(info_url, sizeof(info_url), "%sproject=%s&collector=%s",
           broker->info_url, project, collector);

  // Get the broker reponse and check for errors
	io_t *info_chk_err = wandio_create(info_url);
	char info_check_rst[RPKI_BROKER_URL_LEN] = "";
	if (info_chk_err == NULL) {
	  std_print("ERROR: Could not open %s for reading\n", info_url);
	  wandio_destroy(info_chk_err);
    return -1;
	}
	wandio_read(info_chk_err, info_check_rst, sizeof(info_check_rst));
	if(!strncmp(info_check_rst, "Error:", strlen("Error:")) || 
     !strncmp(info_check_rst, "Malformed", strlen("Malformed"))) {
    info_check_rst[strlen(info_check_rst)] = '\0';
	  std_print("%s\n", info_check_rst);
	  wandio_destroy(info_chk_err);
    return -1;
	}

	wandio_destroy(info_chk_err);
  strncpy(broker->info_host, strtok(info_check_rst, ":"), sizeof(broker->info_host));
  strncpy(broker->info_port, strtok(NULL, ":"), sizeof(broker->info_port)); 

  // Start the RTR connection (with SSH if required)
  config_rtr_t *rtr = &cfg->cfg_rtr;
  if(ssh_options == NULL) {
    rtr->rtr_mgr_cfg = live_validation_start_connection(cfg, broker->info_host, broker->info_port, 
                                                        NULL, NULL, NULL);
    if(rtr->rtr_mgr_cfg == NULL) { return -1; } 
    else {
      return 0;
    }
  }
  char* ssh_user = strtok(ssh_options, ",");
  char* ssh_hostkey = strtok(NULL, ",");
  char* ssh_privkey = strtok(NULL, ",");
  rtr->rtr_mgr_cfg = live_validation_start_connection(cfg, broker->info_host, broker->info_port,
                                                      ssh_user, ssh_hostkey, ssh_privkey);

  if(rtr->rtr_mgr_cfg == NULL) { return -1; }
  else {
    return 0;
  }
}

struct rtr_mgr_config *live_validation_start_connection(rpki_cfg_t* cfg, char *host, char *port, 
                              char *ssh_user, char *ssh_hostkey, char *ssh_privkey){

  struct tr_socket *tr = malloc(sizeof(struct tr_socket));
  debug_print("Live mode (host: %s, port: %s)\n", host, port);

  // If all SSH options are syntactically valid, build a SSH config else build a TCP config
  if (ssh_user != NULL && ssh_hostkey != NULL && ssh_privkey != NULL) {
#ifdef WITH_SSH
    int ssh_port = atoi(port);
    struct tr_ssh_config config = {host, ssh_port, NULL, ssh_user, ssh_hostkey, ssh_privkey};
    tr_ssh_init(&config, tr);

    if (tr_open(tr) == TR_ERROR) {
      std_print("%s", "ERROR: Could not initialising the SSH socket, invalid SSH options\n");
      return NULL;
    }
#else
    std_print("%s", "ERROR: The library was not configured with SSH\n");
    return NULL;
#endif
  } else {
    struct tr_tcp_config config = {host, port, NULL};
    tr_tcp_init(&config, tr);
  }

  // Integrate the configuration into the socket and start the RTR MGR
  struct rtr_socket *rtr = malloc(sizeof(struct rtr_socket));
  rtr->tr_socket = tr;

  struct rtr_mgr_group groups[1];
  groups[0].sockets = malloc(sizeof(struct rtr_socket *));
  groups[0].sockets_len = 1;
  groups[0].sockets[0] = rtr;
  groups[0].preference = 1;

  struct rtr_mgr_config *conf;
  int ret = rtr_mgr_init(&conf, groups, 1, 30, 600, 600, NULL, NULL, NULL, NULL);

  cfg->cfg_broker.live_init = 1;
  rtr_mgr_start(conf);

  while (!rtr_mgr_conf_in_sync(conf))
    sleep(1);

  return conf;
}

void live_validation_close_connection(rpki_cfg_t* cfg, struct rtr_mgr_config *mgr_cfg){

  struct tr_socket *tr = mgr_cfg->groups[0].sockets[0]->tr_socket;
  struct rtr_socket *rtr = mgr_cfg->groups[0].sockets[0];
  struct rtr_socket **socket = mgr_cfg->groups[0].sockets;

  // Only close and free the RTR sockets if they were initialized
  if(cfg->cfg_broker.live_init) {
    rtr_mgr_stop(mgr_cfg);
    rtr_mgr_free(mgr_cfg);
    tr_free(tr);
    free(tr);
    free(rtr);
    free(socket);
  }
}

struct reasoned_result live_validate_reason(struct rtr_mgr_config *mgr_cfg, uint32_t asn,
                                            char* prefix, uint8_t mask_len){
  struct lrtr_ip_addr pref;
  lrtr_ip_str_to_addr(prefix, &pref);
  enum pfxv_state result;
  struct pfx_record *reason = NULL;
  unsigned int reason_len = 0;

  pfx_table_validate_r(mgr_cfg->groups[0].sockets[0]->pfx_table, &reason,
                       &reason_len, asn, &pref, mask_len, &result);

  struct reasoned_result reasoned_res;
  reasoned_res.reason = reason;
  reasoned_res.reason_len = reason_len;
  reasoned_res.result = result;

  return (reasoned_res);
}
