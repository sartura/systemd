/* SPDX-License-Identifier: LGPL-2.1+ */

#include "sd-dhcp-server.h"

#include "dhcp-server-internal.h"
#include "fd-util.h"
#include "fileio.h"
#include "networkd-dhcp-server.h"
#include "networkd-link.h"
#include "networkd-manager.h"
#include "networkd-network.h"
#include "parse-util.h"
#include "socket-netlink.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"

static Address* link_find_dhcp_server_address(Link *link) {
        Address *address;

        assert(link);
        assert(link->network);

        /* The first statically configured address if there is any */
        LIST_FOREACH(addresses, address, link->network->static_addresses) {

                if (address->family != AF_INET)
                        continue;

                if (in_addr_is_null(address->family, &address->in_addr))
                        continue;

                return address;
        }

        /* If that didn't work, find a suitable address we got from the pool */
        LIST_FOREACH(addresses, address, link->pool_addresses) {
                if (address->family != AF_INET)
                        continue;

                return address;
        }

        return NULL;
}

static int link_push_uplink_dns_to_dhcp_server(Link *link, sd_dhcp_server *s) {
        _cleanup_free_ struct in_addr *addresses = NULL;
        size_t n_addresses = 0, n_allocated = 0;
        unsigned i;

        for (i = 0; i < link->network->n_dns; i++) {
                struct in_addr ia;

                /* Only look for IPv4 addresses */
                if (link->network->dns[i].family != AF_INET)
                        continue;

                ia = link->network->dns[i].address.in;

                /* Never propagate obviously borked data */
                if (in4_addr_is_null(&ia) || in4_addr_is_localhost(&ia))
                        continue;

                if (!GREEDY_REALLOC(addresses, n_allocated, n_addresses + 1))
                        return log_oom();

                addresses[n_addresses++] = ia;
        }

        if (link->network->dhcp_use_dns && link->dhcp_lease) {
                const struct in_addr *da = NULL;
                int j, n;

                n = sd_dhcp_lease_get_dns(link->dhcp_lease, &da);
                if (n > 0) {

                        if (!GREEDY_REALLOC(addresses, n_allocated, n_addresses + n))
                                return log_oom();

                        for (j = 0; j < n; j++)
                                if (in4_addr_is_non_local(&da[j]))
                                        addresses[n_addresses++] = da[j];
                }
        }

        if (n_addresses <= 0)
                return 0;

        return sd_dhcp_server_set_dns(s, addresses, n_addresses);
}

static int link_push_uplink_to_dhcp_server(
                Link *link,
                sd_dhcp_lease_info what,
                sd_dhcp_server *s) {

        _cleanup_free_ struct in_addr *addresses = NULL;
        size_t n_addresses = 0, n_allocated = 0;
        bool lease_condition;
        char **servers;

        if (!link->network)
                return 0;

        log_link_debug(link, "Copying %s from link", dhcp_lease_info_to_string(what));

        switch (what) {
        case SD_DHCP_LEASE_DNS_SERVERS:
                /* DNS servers are stored as parsed data, so special handling is required.
                 * TODO: check if DNS servers should be stored unparsed too. */
                return link_push_uplink_dns_to_dhcp_server(link, s);

        case SD_DHCP_LEASE_NTP_SERVERS:
                servers = link->network->ntp;
                lease_condition = link->network->dhcp_use_ntp;
                break;

        case SD_DHCP_LEASE_POP3_SERVERS:
                servers = link->network->pop3;
                lease_condition = true;
                break;

        case SD_DHCP_LEASE_SMTP_SERVERS:
                servers = link->network->smtp;
                lease_condition = true;
                break;

        case SD_DHCP_LEASE_SIP_SERVERS:
                servers = link->network->sip;
                lease_condition = link->network->dhcp_use_sip;
                break;

        case SD_DHCP_LEASE_LPR_SERVERS:
                servers = link->network->lpr;
                lease_condition = true;
                break;

        default:
                assert_not_reached("Unknown DHCP lease info item");
        }

        char **a;
        STRV_FOREACH(a, servers) {
                union in_addr_union ia;

                /* Only look for IPv4 addresses */
                if (in_addr_from_string(AF_INET, *a, &ia) <= 0)
                        continue;

                /* Never propagate obviously borked data */
                if (in4_addr_is_null(&ia.in) || in4_addr_is_localhost(&ia.in))
                        continue;

                if (!GREEDY_REALLOC(addresses, n_allocated, n_addresses + 1))
                        return log_oom();

                addresses[n_addresses++] = ia.in;
        }

        if (lease_condition && link->dhcp_lease) {
                const struct in_addr *da;

                size_t n = sd_dhcp_lease_get_servers(link->dhcp_lease, what, &da);
                if (n > 0) {
                        if (!GREEDY_REALLOC(addresses, n_allocated, n_addresses + n))
                                return log_oom();

                        for (unsigned i = 0; i < n; i++)
                                if (in4_addr_is_non_local(&da[i]))
                                        addresses[n_addresses++] = da[i];
                }
        }

        if (n_addresses <= 0)
                return 0;

        return sd_dhcp_server_set_servers(s, what, addresses, n_addresses);
}

static int dhcp4_server_parse_dns_server_string_and_warn(Link *l, const char *string, struct in_addr **addresses, size_t *n_allocated, size_t *n_addresses) {
        for (;;) {
                _cleanup_free_ char *word = NULL, *server_name = NULL;
                union in_addr_union address;
                int family, r, ifindex = 0;

                r = extract_first_word(&string, &word, NULL, 0);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                r = in_addr_ifindex_name_from_string_auto(word, &family, &address, &ifindex, &server_name);
                if (r < 0) {
                        log_warning_errno(r, "Failed to parse DNS server address '%s', ignoring: %m", word);
                        continue;
                }

                /* Only look for IPv4 addresses */
                if (family != AF_INET)
                        continue;

                /* Never propagate obviously borked data */
                if (in4_addr_is_null(&address.in) || in4_addr_is_localhost(&address.in))
                        continue;

                if (!GREEDY_REALLOC(*addresses, *n_allocated, *n_addresses + 1))
                        return log_oom();

                (*addresses)[(*n_addresses)++] = address.in;
        }

        return 0;
}

static int dhcp4_server_set_dns_from_resolve_conf(Link *link) {
        _cleanup_free_ struct in_addr *addresses = NULL;
        size_t n_addresses = 0, n_allocated = 0;
        _cleanup_fclose_ FILE *f = NULL;
        int n = 0, r;

        f = fopen(PRIVATE_UPLINK_RESOLV_CONF, "re");
        if (!f) {
                if (errno == ENOENT)
                        return 0;

                return log_warning_errno(errno, "Failed to open " PRIVATE_UPLINK_RESOLV_CONF ": %m");
        }

        for (;;) {
                _cleanup_free_ char *line = NULL;
                const char *a;
                char *l;

                r = read_line(f, LONG_LINE_MAX, &line);
                if (r < 0)
                        return log_error_errno(r, "Failed to read " PRIVATE_UPLINK_RESOLV_CONF ": %m");
                if (r == 0)
                        break;

                n++;

                l = strstrip(line);
                if (IN_SET(*l, '#', ';', 0))
                        continue;

                a = first_word(l, "nameserver");
                if (!a)
                        continue;

                r = dhcp4_server_parse_dns_server_string_and_warn(link, a, &addresses, &n_allocated, &n_addresses);
                if (r < 0)
                        log_warning_errno(r, "Failed to parse DNS server address '%s', ignoring.", a);
        }

        if (n_addresses <= 0)
                return 0;

        return sd_dhcp_server_set_dns(link->dhcp_server, addresses, n_addresses);
}

int dhcp4_server_configure(Link *link) {
        bool acquired_uplink = false;
        sd_dhcp_option *p;
        sd_dhcp_static_lease *s;
        Link *uplink = NULL;
        Address *address;
        Iterator i;
        int r;

        address = link_find_dhcp_server_address(link);
        if (!address)
                return log_link_error_errno(link, SYNTHETIC_ERRNO(EBUSY),
                                            "Failed to find suitable address for DHCPv4 server instance.");

        /* use the server address' subnet as the pool */
        r = sd_dhcp_server_configure_pool(link->dhcp_server, &address->in_addr.in, address->prefixlen,
                                          link->network->dhcp_server_pool_offset, link->network->dhcp_server_pool_size);
        if (r < 0)
                return log_link_error_errno(link, r, "Failed to configure address pool for DHCPv4 server instance: %m");

        /* TODO:
        r = sd_dhcp_server_set_router(link->dhcp_server, &main_address->in_addr.in);
        if (r < 0)
                return r;
        */

        if (link->network->dhcp_server_max_lease_time_usec > 0) {
                r = sd_dhcp_server_set_max_lease_time(link->dhcp_server,
                                                      DIV_ROUND_UP(link->network->dhcp_server_max_lease_time_usec, USEC_PER_SEC));
                if (r < 0)
                        return log_link_error_errno(link, r, "Failed to set maximum lease time for DHCPv4 server instance: %m");
        }

        if (link->network->dhcp_server_default_lease_time_usec > 0) {
                r = sd_dhcp_server_set_default_lease_time(link->dhcp_server,
                                                          DIV_ROUND_UP(link->network->dhcp_server_default_lease_time_usec, USEC_PER_SEC));
                if (r < 0)
                        return log_link_error_errno(link, r, "Failed to set default lease time for DHCPv4 server instance: %m");
        }

        const struct {
                bool condition;
                const struct in_addr *servers;
                unsigned n_servers;
        } configs[] = {
                [SD_DHCP_LEASE_DNS_SERVERS] = {
                        link->network->dhcp_server_emit_dns,
                        link->network->dhcp_server_dns,
                        link->network->n_dhcp_server_dns,
                },
                [SD_DHCP_LEASE_NTP_SERVERS] = {
                        link->network->dhcp_server_emit_ntp,
                        link->network->dhcp_server_ntp,
                        link->network->n_dhcp_server_ntp,
                },
                [SD_DHCP_LEASE_SIP_SERVERS] = {
                        link->network->dhcp_server_emit_sip,
                        link->network->dhcp_server_sip,
                        link->network->n_dhcp_server_sip,
                },
                [SD_DHCP_LEASE_POP3_SERVERS] = {
                        true,
                        link->network->dhcp_server_pop3,
                        link->network->n_dhcp_server_pop3,
                },
                [SD_DHCP_LEASE_SMTP_SERVERS] = {
                        true,
                        link->network->dhcp_server_smtp,
                        link->network->n_dhcp_server_smtp,
                },
                [SD_DHCP_LEASE_LPR_SERVERS] = {
                        true,
                        link->network->dhcp_server_lpr,
                        link->network->n_dhcp_server_lpr,
                },
        };
        assert_cc(ELEMENTSOF(configs) == _SD_DHCP_LEASE_INFO_MAX);

        for (unsigned n = 0; n < ELEMENTSOF(configs); n++)
                if (configs[n].condition) {
                        if (configs[n].n_servers > 0)
                                r = sd_dhcp_server_set_servers(link->dhcp_server, n,
                                                               configs[n].servers, configs[n].n_servers);
                        else {
                                if (!acquired_uplink) {
                                        uplink = manager_find_uplink(link->manager, link);
                                        acquired_uplink = true;
                                }

                                if (!uplink) {
                                        log_link_debug(link,
                                                       "Not emitting %s on link, couldn't find suitable uplink.",
                                                       dhcp_lease_info_to_string(n));
                                        r = 0;
                                } else if (uplink->network)
                                        r = link_push_uplink_to_dhcp_server(uplink, n, link->dhcp_server);
                                else if (n == SD_DHCP_LEASE_DNS_SERVERS)
                                        r = dhcp4_server_set_dns_from_resolve_conf(link);
                        }
                        if (r < 0)
                                log_link_warning_errno(link, r,
                                                       "Failed to set %s for DHCP server, ignoring: %m",
                                                       dhcp_lease_info_to_string(n));
                }

        r = sd_dhcp_server_set_emit_router(link->dhcp_server, link->network->dhcp_server_emit_router);
        if (r < 0)
                return log_link_error_errno(link, r, "Failed to set router emission for DHCP server: %m");

        if (link->network->dhcp_server_emit_timezone) {
                _cleanup_free_ char *buffer = NULL;
                const char *tz;

                if (link->network->dhcp_server_timezone)
                        tz = link->network->dhcp_server_timezone;
                else {
                        r = get_timezone(&buffer);
                        if (r < 0)
                                return log_link_error_errno(link, r, "Failed to determine timezone: %m");

                        tz = buffer;
                }

                r = sd_dhcp_server_set_timezone(link->dhcp_server, tz);
                if (r < 0)
                        return log_link_error_errno(link, r, "Failed to set timezone for DHCP server: %m");
        }

        ORDERED_HASHMAP_FOREACH(p, link->network->dhcp_server_send_options, i) {
                r = sd_dhcp_server_add_option(link->dhcp_server, p);
                if (r == -EEXIST)
                        continue;
                if (r < 0)
                        return log_link_error_errno(link, r, "Failed to set DHCPv4 option: %m");
        }

        ORDERED_HASHMAP_FOREACH(p, link->network->dhcp_server_send_vendor_options, i) {
                r = sd_dhcp_server_add_vendor_option(link->dhcp_server, p);
                if (r == -EEXIST)
                        continue;
                if (r < 0)
                        return log_link_error_errno(link, r, "Failed to set DHCPv4 option: %m");
        }
        HASHMAP_FOREACH(s, link->network->dhcp_static_leases, i) {
                r = sd_dhcp_server_add_static_lease(link->dhcp_server, s);
                if (r == -EEXIST)
                        continue;
                if (r < 0)
                        return log_link_error_errno(link, r, "Failed to set DHCPv4 static lease to for DHCP server: %m");
        }

        if (!sd_dhcp_server_is_running(link->dhcp_server)) {
                r = sd_dhcp_server_start(link->dhcp_server);
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not start DHCPv4 server instance: %m");
        }

        return 0;
}

static int config_parse_dhcp_lease_server_list(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *lvalue,
                const char *rvalue,
                struct in_addr **addresses,
                unsigned *n_addresses) {

        assert(filename);
        assert(lvalue);
        assert(rvalue);

        for (const char *p = rvalue;;) {
                _cleanup_free_ char *w = NULL;
                union in_addr_union a;
                int r;

                r = extract_first_word(&p, &w, NULL, 0);
                if (r == -ENOMEM)
                        return log_oom();
                if (r < 0) {
                        log_syntax(unit, LOG_ERR, filename, line, r,
                                   "Failed to extract word, ignoring: %s", rvalue);
                        return 0;
                }
                if (r == 0)
                        return 0;

                r = in_addr_from_string(AF_INET, w, &a);
                if (r < 0) {
                        log_syntax(unit, LOG_ERR, filename, line, r,
                                   "Failed to parse %s= address '%s', ignoring: %m", lvalue, w);
                        continue;
                }

                struct in_addr *m = reallocarray(*addresses, *n_addresses + 1, sizeof(struct in_addr));
                if (!m)
                        return log_oom();

                m[(*n_addresses)++] = a.in;
                *addresses = m;
        }
}

int config_parse_dhcp_server_dns(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Network *n = data;

        return config_parse_dhcp_lease_server_list(unit, filename, line,
                                                   lvalue, rvalue,
                                                   &n->dhcp_server_dns, &n->n_dhcp_server_dns);
}

int config_parse_dhcp_server_ntp(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Network *n = data;

        return config_parse_dhcp_lease_server_list(unit, filename, line,
                                                   lvalue, rvalue,
                                                   &n->dhcp_server_ntp, &n->n_dhcp_server_ntp);
}

int config_parse_dhcp_server_sip(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Network *n = data;

        return config_parse_dhcp_lease_server_list(unit, filename, line,
                                                   lvalue, rvalue,
                                                   &n->dhcp_server_sip, &n->n_dhcp_server_sip);
}

int config_parse_dhcp_server_pop3_servers(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Network *n = data;

        return config_parse_dhcp_lease_server_list(unit, filename, line,
                                                   lvalue, rvalue,
                                                   &n->dhcp_server_pop3, &n->n_dhcp_server_pop3);
}

int config_parse_dhcp_server_smtp_servers(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Network *n = data;

        return config_parse_dhcp_lease_server_list(unit, filename, line,
                                                   lvalue, rvalue,
                                                   &n->dhcp_server_smtp, &n->n_dhcp_server_smtp);

}

int config_parse_dhcp_server_lpr_servers(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Network *n = data;

        return config_parse_dhcp_lease_server_list(unit, filename, line,
                                                   lvalue, rvalue,
                                                   &n->dhcp_server_lpr, &n->n_dhcp_server_lpr);

}

static void dhcp_client_id_freep(DHCPClientId **p) {
        if (!*p)
                return;

        free((*p)->data);
        free(*p);
}

int config_parse_dhcp_static_leases(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(sd_dhcp_static_lease_unrefp) sd_dhcp_static_lease *lease = NULL;
        _cleanup_free_ char *word = NULL;
        _cleanup_free_ struct ether_addr *n = NULL;
        OrderedHashmap **static_leases = data;
        union in_addr_union addr;
        const char *p;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                *static_leases = ordered_hashmap_free(*static_leases);
                return 0;
        }

        p = rvalue;
        r = extract_first_word(&p, &word, " ", 0);
        if (r == -ENOMEM)
                return log_oom();
        if (r <= 0) {
                log_syntax(unit, LOG_ERR, filename, line, r,
                           "Invalid hardware address, ignoring assignment: %s", rvalue);
                return 0;
        }

        n = new(struct ether_addr, 1);
        if (!n)
                return log_oom();

        r = ether_addr_from_string(word, n);
        if (r < 0) {
                log_syntax(unit, LOG_ERR, filename, line, r,
                           "Not a valid MAC address, ignoring: %s", word);
                return 0;
        }

        word = mfree(word);
        r = extract_first_word(&p, &word, " ", 0);
        if (r == -ENOMEM)
                return log_oom();
        if (r <= 0) {
                log_syntax(unit, LOG_ERR, filename, line, r,
                           "Invalid IP address, ignoring assignment: %s", rvalue);
                return 0;
        }

        r = in_addr_from_string(AF_INET, word, &addr);
        if (r < 0) {
                log_syntax(unit, LOG_ERR, filename, line, r,
                           "Failed to parse DHCPv4 IPv4 address data, ignoring assignment: %s", word);
                return 0;
        }

        _cleanup_(dhcp_client_id_freep) DHCPClientId *c = new(DHCPClientId, 1);
        if (!c)
                return log_oom();
        c->data = malloc(ETH_ALEN + 1);
        if (!c->data)
                return log_oom();
        ((uint8_t*)  c->data)[0] = 0x01;
        memcpy((uint8_t*) c->data + 1, n->ether_addr_octet, ETH_ALEN);
        c->length = ETH_ALEN + 1;

        r = sd_dhcp_static_lease_new(c, &addr.in, &lease);
        if (r < 0) {
                log_syntax(unit, LOG_ERR, filename, line, r,
                           "Failed to store DHCPv4 static lease '%s', ignoring assignment: %m", rvalue);
                return 0;
        }

        r = ordered_hashmap_ensure_allocated(static_leases, &dhcp_static_leases_hash_ops);
        if (r < 0)
                return log_oom();

        /* Overwrite existing option */
        r = ordered_hashmap_replace(*static_leases, c, lease);
        if (r < 0) {
                log_syntax(unit, LOG_ERR, filename, line, r,
                           "Failed to store DHCPv4 static lease '%s', ignoring assignment: %m", rvalue);
                return 0;
        }

        sd_dhcp_static_lease *removed_lease = NULL;
        removed_lease = ordered_hashmap_remove(*static_leases, c);
        if (removed_lease == NULL) {
                log_syntax(unit, LOG_ERR, filename, line, r,
                           "Failed to remove DHCPv4 static lease '%s', ignoring assignment: %m", rvalue);
                return 0;
        }

        return 0;
}
