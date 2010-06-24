
/*
 * Copyright (C) 2010 Jerome Loyet (jerome at loyet dot net)
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_http_sticky_misc.h"

typedef struct {
	ngx_str_t        digest;
	struct sockaddr *sockaddr;
	socklen_t        socklen;
	ngx_str_t       *name;
} ngx_http_sticky_peer_t;

typedef struct {
	ngx_http_upstream_rr_peers_t rr_peers;
	ngx_uint_t  number;
	ngx_http_sticky_peer_t *peer;
} ngx_http_sticky_peers_t;

typedef struct {
	ngx_http_upstream_srv_conf_t  uscf;
	ngx_str_t                     cookie_name;
	ngx_str_t                     cookie_domain;
	ngx_str_t                     cookie_path;
	time_t                        cookie_expires;
	ngx_str_t                     hmac_key;
	ngx_http_sticky_misc_hash_pt  hash;
	ngx_http_sticky_misc_hmac_pt  hmac;
	ngx_http_sticky_peers_t  *peers;
} ngx_http_sticky_srv_conf_t;

typedef struct {
	ngx_http_upstream_rr_peer_data_t   rrp;
	ngx_http_request_t                 *r;
	ngx_str_t                          route;
	ngx_flag_t                         tried_route;
	ngx_http_sticky_srv_conf_t         *sticky_cf;
} ngx_http_sticky_peer_data_t;

static ngx_int_t  ngx_http_sticky_ups_init_peer (ngx_http_request_t *r, ngx_http_upstream_srv_conf_t *us);
static ngx_int_t  ngx_http_sticky_ups_get       (ngx_peer_connection_t *pc, void *data);
static char      *ngx_http_sticky_set           (ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void      *ngx_http_sticky_create_conf   (ngx_conf_t *cf);

static ngx_command_t  ngx_http_sticky_commands[] = {
	{ ngx_string("sticky"),
		NGX_HTTP_UPS_CONF|NGX_CONF_ANY,
		ngx_http_sticky_set,
		0,
		0,
		NULL },

	ngx_null_command
};

static ngx_http_module_t  ngx_http_sticky_module_ctx = {
	NULL,                                  /* preconfiguration */
	NULL,                                  /* postconfiguration */
	NULL,                                  /* create main configuration */
	NULL,                                  /* init main configuration */
	ngx_http_sticky_create_conf,           /* create server configuration */
	NULL,                                  /* merge server configuration */
	NULL,                                  /* create location configuration */
	NULL                                   /* merge location configuration */
};

ngx_module_t  ngx_http_sticky_module = {
	NGX_MODULE_V1,
	&ngx_http_sticky_module_ctx, /* module context */
	ngx_http_sticky_commands,    /* module directives */
	NGX_HTTP_MODULE,                       /* module type */
	NULL,                                  /* init master */
	NULL,                                  /* init module */
	NULL,                                  /* init process */
	NULL,                                  /* init thread */
	NULL,                                  /* exit thread */
	NULL,                                  /* exit process */
	NULL,                                  /* exit master */
	NGX_MODULE_V1_PADDING
};

ngx_int_t ngx_http_sticky_ups_init(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *us)
{
	ngx_http_upstream_rr_peers_t *rr_peers;
	ngx_http_sticky_peer_t *peer;
	ngx_http_sticky_srv_conf_t *conf;
	ngx_uint_t i, n;

	if (ngx_http_upstream_init_round_robin(cf, us) != NGX_OK) {
		return NGX_ERROR;
	}

	rr_peers = us->peer.data;
	n = 0;
	if (rr_peers->number > 0) n = rr_peers->number;
	if (rr_peers->next && rr_peers->next->number > 0) n += rr_peers->next->number;

	if (n <= 0) {
		/* in this case sticky has nothing to do. Let RR handle everything. I give up */
		return NGX_OK;
	}

	conf = ngx_http_conf_upstream_srv_conf(us, ngx_http_sticky_module);

	/* create our own upstream indexes */
	conf->peers = ngx_pcalloc(cf->pool, sizeof(ngx_http_sticky_peers_t));
	if (conf->peers == NULL) {
		return NGX_ERROR;
	}

	conf->peers->peer = ngx_pcalloc(cf->pool, sizeof(ngx_http_sticky_peer_data_t) * n);
	if (conf->peers->peer == NULL) {
		return NGX_ERROR;
	}

	n = 0;
	peer = conf->peers->peer;

	/* register normal peers */
	if (rr_peers->number > 0) {
		conf->peers->number += rr_peers->number;
		for (i=0; i < rr_peers->number; i++, n++) {
			if (conf->hash) {
				conf->hash(cf->pool, rr_peers->peer[i].sockaddr, rr_peers->peer[i].socklen, &peer[n].digest);
			} else if (conf->hmac && conf->hmac_key.len > 0) {
				conf->hmac(cf->pool, rr_peers->peer[i].sockaddr, rr_peers->peer[i].socklen, &conf->hmac_key, &peer[n].digest);
			}
			peer[n].sockaddr = rr_peers->peer[i].sockaddr;
			peer[n].socklen = rr_peers->peer[i].socklen;
			peer[n].name = &rr_peers->peer[i].name;
		}
	}

	/* register backup peers */
	if (rr_peers->next && rr_peers->next->number > 0) {
		conf->peers->number += rr_peers->next->number;
		for (i=0; i < rr_peers->next->number; i++, n++) {
			if (conf->hash) {
				conf->hash(cf->pool, rr_peers->next->peer[i].sockaddr, rr_peers->next->peer[i].socklen, &peer[n].digest);
			} else if (conf->hmac && conf->hmac_key.len > 0) {
				conf->hmac(cf->pool, rr_peers->peer[i].sockaddr, rr_peers->peer[i].socklen, &conf->hmac_key, &peer[n].digest);
			}
			peer[n].sockaddr = rr_peers->next->peer[i].sockaddr;
			peer[n].socklen = rr_peers->next->peer[i].socklen;
			peer[n].name = &rr_peers->peer[i].name;
		}
	}

	/* declare next custom function */
	us->peer.init = ngx_http_sticky_ups_init_peer;

	return NGX_OK;
}

static ngx_int_t ngx_http_sticky_ups_init_peer(ngx_http_request_t *r, ngx_http_upstream_srv_conf_t *us)
{
	ngx_http_sticky_peer_data_t *spd;
	ngx_http_upstream_rr_peer_data_t *rrp;

	spd = ngx_palloc(r->pool, sizeof(ngx_http_sticky_peer_data_t));
	if (spd == NULL) {
		return NGX_ERROR;
	}

	spd->sticky_cf = ngx_http_conf_upstream_srv_conf(us, ngx_http_sticky_module);
	spd->tried_route = 1; /* presume a route cookie has not been found */

	if (ngx_http_parse_multi_header_lines(&r->headers_in.cookies, &spd->sticky_cf->cookie_name, &spd->route) != NGX_DECLINED) {
		/* a route cookie has been found. Let's give it a try */
		spd->tried_route = 0;
		ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "[sticky/ups_init_peer] got cookie route=%V", &spd->route);
	}

	/* lets nginx RR init itself */
	if (ngx_http_upstream_init_round_robin_peer(r, us) != NGX_OK) {
		return NGX_ERROR;
	}

	/* override the nginx RR structure with our own without overlapping it */
	rrp = r->upstream->peer.data;
	spd->rrp = *rrp;
	spd->r = r;
	r->upstream->peer.data = &spd->rrp;

	/* register next custom function */
	/* other functions stay in the RR core scope */
	r->upstream->peer.get = ngx_http_sticky_ups_get;

	return NGX_OK;
}

static ngx_int_t ngx_http_sticky_ups_get(ngx_peer_connection_t *pc, void *data)
{
	ngx_uint_t i;
	ngx_http_sticky_peer_data_t *spd = data;
	ngx_http_sticky_srv_conf_t *conf = spd->sticky_cf;
	ngx_http_sticky_peer_t *peer;

	if (!spd->tried_route) {
		spd->tried_route = 1;
		if (spd->route.len > 0) {
			/* we got a route and we never tried it. Let's use it first! */
			ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[sticky/ups_get] We got a route and never tried it. TRY IT !");

			if (conf->hash || conf->hmac) {
				/* if hashing, find the corresponding peer and use it ! */
				for (i=0; i < conf->peers->number; i++) {
					peer = &conf->peers->peer[i];
					if (ngx_strncmp(spd->route.data, peer->digest.data, peer->digest.len) != 0) {
						continue;
					}
					pc->sockaddr = peer->sockaddr;
					pc->socklen = peer->socklen;
					pc->name = peer->name;
					ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[sticky/ups_get] peer \"%V\" with digest \"%V\" matches at index %d", peer->name, &peer->digest, i);
					return NGX_OK;
				}
			} else {
				/* if indexing, directely use the corresponding peer if it exists */
				ngx_int_t index = ngx_atoi(spd->route.data, spd->route.len);

				if (index != NGX_ERROR && index >= 0 && index < (ngx_int_t)conf->peers->number) {
					peer = &conf->peers->peer[index];
					ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[sticky/ups_get] peer \"%V\" matches at index %d", peer->name, index);
					pc->sockaddr = peer->sockaddr;
					pc->socklen = peer->socklen;
					pc->name = peer->name;
					return NGX_OK;

				} else {
					ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[sticky/ups_get] cookie \"%V\" is not valid (%d)", &spd->route, index);
				}
			}
		}
	}

	/* switch back to classic rr */
	if ((i = ngx_http_upstream_get_round_robin_peer(pc, data)) != NGX_OK) {
		return i;
	}

	/* found the used peer to set the cookie */
	/* We're here because the nginx RR module choosed one */
	for (i=0; i < conf->peers->number; i++) {
		peer = &conf->peers->peer[i];

		if (peer->sockaddr == pc->sockaddr && peer->socklen == pc->socklen) {
			if (conf->hash || conf->hmac) {
				ngx_http_sticky_misc_set_cookie(spd->r, &conf->cookie_name, &conf->peers->peer[i].digest, &conf->cookie_domain, &conf->cookie_path, conf->cookie_expires);
				ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[sticky/ups_get] set cookie \"%V\" value=\"%V\" index=%d", &conf->cookie_name, &conf->peers->peer[i].digest, i);
			} else {
				ngx_str_t route;
				ngx_uint_t tmp = i;
				route.len = 0;
				do {
					route.len++;
				} while (tmp /= 10);
				route.data = ngx_pcalloc(spd->r->pool, sizeof(u_char) * (route.len + 1));
				if (route.data == NULL) {
					return NGX_ERROR;
				}
				ngx_snprintf(route.data, route.len, "%d", i);
				route.len = ngx_strlen(route.data);
				ngx_http_sticky_misc_set_cookie(spd->r, &conf->cookie_name, &route, &conf->cookie_domain, &conf->cookie_path, conf->cookie_expires);
			}
			return NGX_OK; /* found and hopefully the cookie have been set */
		}
	}

	return NGX_OK;
}

static char *ngx_http_sticky_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_http_upstream_srv_conf_t  *uscf;
	ngx_http_sticky_srv_conf_t *usscf;
	ngx_uint_t i;
	ngx_str_t tmp;
	ngx_str_t name = ngx_string("route");
	ngx_str_t domain = ngx_string("");
	ngx_str_t path = ngx_string("");
	ngx_str_t hmac_key = ngx_string("");
	time_t expires = NGX_CONF_UNSET;
	ngx_http_sticky_misc_hash_pt hash = NGX_CONF_UNSET_PTR;
	ngx_http_sticky_misc_hmac_pt hmac = NULL;

	for (i=1; i<cf->args->nelts; i++) {
		ngx_str_t *value = cf->args->elts;

		if ((u_char *)ngx_strstr(value[i].data, "name=") == value[i].data) {
			if (value[i].len <= sizeof("name=") - 1) {
				ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "a value must be provided to \"name=\"");
				return NGX_CONF_ERROR;
			}
			name.len = value[i].len - ngx_strlen("name=");
			name.data = (u_char *)(value[i].data + sizeof("name=") - 1);
			continue;
		}

		if ((u_char *)ngx_strstr(value[i].data, "domain=") == value[i].data) {
			if (value[i].len <= ngx_strlen("domain=")) {
				ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "a value must be provided to \"domain=\"");
				return NGX_CONF_ERROR;
			}
			domain.len = value[i].len - ngx_strlen("domain=");
			domain.data = (u_char *)(value[i].data + sizeof("domain=") - 1);
			continue;
		}

		if ((u_char *)ngx_strstr(value[i].data, "path=") == value[i].data) {
			if (value[i].len <= ngx_strlen("path=")) {
				ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "a value must be provided to \"path=\"");
				return NGX_CONF_ERROR;
			}
			path.len = value[i].len - ngx_strlen("path=");
			path.data = (u_char *)(value[i].data + sizeof("path=") - 1);
			continue;
		}

		if ((u_char *)ngx_strstr(value[i].data, "expires=") == value[i].data) {
			if (value[i].len <= sizeof("expires=") - 1) {
				ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "a value must be provided to \"expires=\"");
				return NGX_CONF_ERROR;
			}
			tmp.len =  value[i].len - ngx_strlen("expires=");
			tmp.data = (u_char *)(value[i].data + sizeof("expires=") - 1);
			expires = ngx_parse_time(&tmp, 1);
			if (expires == NGX_ERROR || expires < 1) {
				ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "invalid value for \"expires=\"");
				return NGX_CONF_ERROR;
			}
			continue;
		}
	
		if ((u_char *)ngx_strstr(value[i].data, "hash=") == value[i].data) {
			if (hmac) {
				ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "please choose between \"hash=\" and \"hmac=\"");
				return NGX_CONF_ERROR;
			}
			if (value[i].len <= sizeof("hash=") - 1) {
				ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "a value must be provided to \"hash=\"");
				return NGX_CONF_ERROR;
			}
			tmp.len =  value[i].len - ngx_strlen("hash=");
			tmp.data = (u_char *)(value[i].data + sizeof("hash=") - 1);
			if (ngx_strncmp(tmp.data, "index", sizeof("index") - 1) == 0 ) {
				hash = NULL;
				continue;
			}
			if (ngx_strncmp(tmp.data, "md5", sizeof("md5") - 1) == 0 ) {
				hash = ngx_http_sticky_misc_md5;
				continue;
			}
			if (ngx_strncmp(tmp.data, "sha1", sizeof("sha1") - 1) == 0 ) {
				hash = ngx_http_sticky_misc_sha1;
				continue;
			}
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "wrong value for \"hash=\": index, md5 or sha1");
			return NGX_CONF_ERROR;
		}
	
		if ((u_char *)ngx_strstr(value[i].data, "hmac=") == value[i].data) {
			if (hash != NGX_CONF_UNSET_PTR) {
				ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "please choose between \"hash=\" and \"hmac=\"");
				return NGX_CONF_ERROR;
			}
			if (value[i].len <= sizeof("hmac=") - 1) {
				ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "a value must be provided to \"hmac=\"");
				return NGX_CONF_ERROR;
			}
			tmp.len =  value[i].len - ngx_strlen("hmac=");
			tmp.data = (u_char *)(value[i].data + sizeof("hmac=") - 1);
			if (ngx_strncmp(tmp.data, "md5", sizeof("md5") - 1) == 0 ) {
				hmac = ngx_http_sticky_misc_hmac_md5;
				continue;
			}
			if (ngx_strncmp(tmp.data, "sha1", sizeof("sha1") - 1) == 0 ) {
				hmac = ngx_http_sticky_misc_hmac_sha1;
				continue;
			}
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "wrong value for \"hmac=\": md5 or sha1");
			return NGX_CONF_ERROR;
		}

		if ((u_char *)ngx_strstr(value[i].data, "hmac_key=") == value[i].data) {
			if (value[i].len <= ngx_strlen("hmac_key=")) {
				ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "a value must be provided to \"hmac_key=\"");
				return NGX_CONF_ERROR;
			}
			hmac_key.len = value[i].len - ngx_strlen("hmac_key=");
			hmac_key.data = (u_char *)(value[i].data + sizeof("hmac_key=") - 1);
			continue;
		}

		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "invalid arguement (%V)", &value[i]);
		return NGX_CONF_ERROR;
	}

	if (hash == NGX_CONF_UNSET_PTR && hmac == NULL) {
		hash = ngx_http_sticky_misc_md5;
	}

	if (hmac_key.len > 0 && hash != NGX_CONF_UNSET_PTR) {
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "\"hmac_key=\" is meaningless when \"hmac\" is used. Please remove it.");
		return NGX_CONF_ERROR;
	}

	if (hmac_key.len == 0 && hmac != NULL) {
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "please specify \"hmac_key=\" when using \"hmac\"");
		return NGX_CONF_ERROR;
	}

	if (hash == NGX_CONF_UNSET_PTR) {
		hash = NULL;
	}

	uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
	uscf->peer.init_upstream = ngx_http_sticky_ups_init;
	uscf->flags = NGX_HTTP_UPSTREAM_CREATE
		|NGX_HTTP_UPSTREAM_WEIGHT
		|NGX_HTTP_UPSTREAM_MAX_FAILS
		|NGX_HTTP_UPSTREAM_FAIL_TIMEOUT
		|NGX_HTTP_UPSTREAM_DOWN
		|NGX_HTTP_UPSTREAM_BACKUP;

	usscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_sticky_module);
	usscf->cookie_name = name;
	usscf->cookie_domain = domain;
	usscf->cookie_path = path;
	usscf->cookie_expires = expires;
	usscf->hash = hash;
	usscf->hmac = hmac;
	usscf->hmac_key = hmac_key;
	
	return NGX_CONF_OK;
}

static void *ngx_http_sticky_create_conf(ngx_conf_t *cf)
{
	ngx_http_sticky_srv_conf_t *conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_sticky_srv_conf_t));
	if (conf == NULL) {
		return NGX_CONF_ERROR;
	}

	return conf;
}
