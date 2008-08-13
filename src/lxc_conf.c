/*
 * Copyright IBM Corp. 2008
 *
 * lxc_conf.c: config functions for managing linux containers
 *
 * Authors:
 *  David L. Leskovec <dlesko at linux.vnet.ibm.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 */

/* includes */
#include <config.h>

#ifdef WITH_LXC

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/uri.h>
#include <libxml/xpath.h>

#include "buf.h"
#include "util.h"
#include "uuid.h"
#include "xml.h"
#include "memory.h"
#include "lxc_conf.h"

/* debug macros */
#define DEBUG(fmt,...) VIR_DEBUG(__FILE__, fmt, __VA_ARGS__)
#define DEBUG0(msg) VIR_DEBUG(__FILE__, "%s", msg)

/* Functions */
void lxcError(virConnectPtr conn, virDomainPtr dom, int code,
              const char *fmt, ...)
{
    va_list args;
    char errorMessage[LXC_MAX_ERROR_LEN];
    const char *codeErrorMessage;

    if (fmt) {
        va_start(args, fmt);
        vsnprintf(errorMessage, LXC_MAX_ERROR_LEN-1, fmt, args);
        va_end(args);
    } else {
        errorMessage[0] = '\0';
    }

    codeErrorMessage = __virErrorMsg(code, fmt);
    __virRaiseError(conn, dom, NULL, VIR_FROM_LXC, code, VIR_ERR_ERROR,
                    codeErrorMessage, errorMessage, NULL, 0, 0,
                    codeErrorMessage, errorMessage);
}

/**
 * lxcParseInterfaceXML:
 * @conn: pointer to connection
 * @nodePtr: pointer to xml node structure
 * @vm: pointer to net definition structure to fill in
 *
 * Parses the XML for a network interface and places the configuration
 * in the given structure.
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcParseInterfaceXML(virConnectPtr conn, xmlNodePtr nodePtr,
                                lxc_net_def_t *netDef)
{
    int rc = -1;
    xmlNodePtr cur;
    xmlChar *type = NULL;
    xmlChar *parentIfName = NULL;
    xmlChar *network = NULL;
    xmlChar *bridge = NULL;
    xmlChar *macaddr = NULL;

    netDef->type = LXC_NET_NETWORK;

    type = xmlGetProp(nodePtr, BAD_CAST "type");
    if (type != NULL) {
        if (xmlStrEqual(type, BAD_CAST "network")) {
            netDef->type = LXC_NET_NETWORK;
        }
        else if (xmlStrEqual(type, BAD_CAST "bridge")) {
            netDef->type = LXC_NET_BRIDGE;
        }
        else {
            lxcError(conn, NULL, VIR_ERR_XML_ERROR,
                     _("invalid interface type: %s"), type);
            goto error_out;
        }
    }

    cur = nodePtr->children;
    for (cur = nodePtr->children; cur != NULL; cur = cur->next) {
        if (cur->type == XML_ELEMENT_NODE) {
            DEBUG("cur->name: %s", (char*)(cur->name));
            if ((macaddr == NULL) &&
                (xmlStrEqual(cur->name, BAD_CAST "mac"))) {
                macaddr = xmlGetProp(cur, BAD_CAST "address");
            } else if ((network == NULL) &&
                       (netDef->type == LXC_NET_NETWORK) &&
                       (xmlStrEqual(cur->name, BAD_CAST "source"))) {
                network = xmlGetProp(cur, BAD_CAST "network");
                parentIfName = xmlGetProp(cur, BAD_CAST "dev");
            } else if ((bridge == NULL) &&
                       (netDef->type == LXC_NET_BRIDGE) &&
                       (xmlStrEqual(cur->name, BAD_CAST "source"))) {
                bridge = xmlGetProp(cur, BAD_CAST "bridge");
            } else if ((parentIfName == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "target"))) {
                parentIfName = xmlGetProp(cur, BAD_CAST "dev");
            }
        }
    }

    if (netDef->type == LXC_NET_NETWORK) {
        if (network == NULL) {
            lxcError(conn, NULL, VIR_ERR_XML_ERROR,
                     _("No <source> 'network' attribute specified with <interface type='network'/>"));
            goto error_out;
        }

        netDef->txName = strdup((char *)network);
        if (NULL == netDef->txName) {
            lxcError(conn, NULL, VIR_ERR_NO_MEMORY,
                     _("No storage for network name"));
            goto error_out;
        }

    } else if (netDef->type == LXC_NET_BRIDGE) {
        if (bridge == NULL) {
            lxcError(conn, NULL, VIR_ERR_XML_ERROR,
                     _("No <source> 'bridge' attribute specified with <interface type='bridge'/>"));
            goto error_out;
        }

        netDef->txName = strdup((char *)bridge);
        if (NULL == netDef->txName) {
            lxcError(conn, NULL, VIR_ERR_NO_MEMORY,
                     _("No storage for bridge name"));
            goto error_out;
        }
    }

    if (parentIfName != NULL) {
        DEBUG("set netDef->parentVeth: %s", netDef->parentVeth);
        netDef->parentVeth = strdup((char *)parentIfName);
        if (NULL == netDef->parentVeth) {
            lxcError(conn, NULL, VIR_ERR_NO_MEMORY,
                     _("No storage for parent veth device name"));
            goto error_out;
        }
    } else {
        netDef->parentVeth = NULL;
        DEBUG0("set netDef->parentVeth: NULL");
    }

    rc = 0;

error_out:
    xmlFree(macaddr);
    xmlFree(network);
    xmlFree(bridge);
    xmlFree(parentIfName);

    return rc;
}

/**
 * lxcParseDomainInterfaces:
 * @conn: pointer to connection
 * @nets: on success, points to an list of net def structs
 * @contextPtr: pointer to xml context
 *
 * Parses the domain network interfaces and returns the information in a list
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcParseDomainInterfaces(virConnectPtr conn,
                                    lxc_net_def_t **nets,
                                    xmlXPathContextPtr contextPtr)
{
    int rc = -1;
    int i;
    lxc_net_def_t *netDef;
    lxc_net_def_t *prevDef = NULL;
    int numNets = 0;
    xmlNodePtr *list;
    int res;

    DEBUG0("parsing nets");

    res = virXPathNodeSet(conn, "/domain/devices/interface", contextPtr, &list);
    if (res > 0) {
        for (i = 0; i < res; ++i) {
            netDef = calloc(1, sizeof(lxc_net_def_t));
            if (NULL == netDef) {
                lxcError(conn, NULL, VIR_ERR_NO_MEMORY,
                         _("No storage for net def structure"));
                free(list);
                goto parse_complete;
            }

            rc = lxcParseInterfaceXML(conn, list[i], netDef);
            if (0 > rc) {
                DEBUG("failed parsing a net: %d", rc);

                free(netDef);
                free(list);
                goto parse_complete;
            }

            DEBUG0("parsed a net");

            /* set the linked list pointers */
            numNets++;
            netDef->next = NULL;
            if (0 == i) {
                *nets = netDef;
            } else {
                prevDef->next = netDef;
            }
            prevDef = netDef;
        }
        free(list);
    }

    rc = numNets;

parse_complete:
    DEBUG("parsed %d nets", rc);
    return rc;
}

static int lxcParseMountXML(virConnectPtr conn, xmlNodePtr nodePtr,
                            lxc_mount_t *lxcMount)
{
    xmlChar *fsType = NULL;
    xmlNodePtr curNode;
    xmlChar *mountSource = NULL;
    xmlChar *mountTarget = NULL;
    int strLen;
    int rc = -1;

    fsType = xmlGetProp(nodePtr, BAD_CAST "type");
    if (NULL == fsType) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("missing filesystem type"));
        goto error;
    }

    if (xmlStrEqual(fsType, BAD_CAST "mount") == 0) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("invalid filesystem type"));
        goto error;
    }

    for (curNode = nodePtr->children;
         NULL != curNode;
         curNode = curNode->next) {
        if (curNode->type != XML_ELEMENT_NODE) {
            continue;
        }

        if ((mountSource == NULL) &&
            (xmlStrEqual(curNode->name, BAD_CAST "source"))) {
            mountSource = xmlGetProp(curNode, BAD_CAST "dir");
        } else if ((mountTarget == NULL) &&
                  (xmlStrEqual(curNode->name, BAD_CAST "target"))) {
            mountTarget = xmlGetProp(curNode, BAD_CAST "dir");
        }
    }

    if (mountSource == NULL) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("missing mount source"));
        goto error;
    }

    strLen = xmlStrlen(mountSource);
    if ((strLen > (PATH_MAX-1)) || (0 == strLen)) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("empty or invalid mount source"));
        goto error;
    }

    strncpy(lxcMount->source, (char *)mountSource, strLen);
    lxcMount->source[strLen] = '\0';

    if (mountTarget == NULL) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("missing mount target"));
        goto error;
    }

    strLen = xmlStrlen(mountTarget);
    if ((strLen > (PATH_MAX-1)) || (0 == strLen)) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("empty or invalid mount target"));
        goto error;
    }

    strncpy(lxcMount->target, (char *)mountTarget, strLen);
    lxcMount->target[strLen] = '\0';

    rc = 0;

error:
    xmlFree(fsType);
    xmlFree(mountSource);
    xmlFree(mountTarget);

    return rc;
}

static int lxcParseDomainName(virConnectPtr conn, char **name,
                              xmlXPathContextPtr contextPtr)
{
    char *res;

    res = virXPathString(conn, "string(/domain/name[1])", contextPtr);
    if (res == NULL) {
        lxcError(conn, NULL, VIR_ERR_NO_NAME, NULL);
        return(-1);
    }

    *name = res;
    return(0);
}

static int lxcParseDomainUUID(virConnectPtr conn, unsigned char *uuid,
                              xmlXPathContextPtr contextPtr)
{
    char *res;

    res = virXPathString(conn, "string(/domain/uuid[1])", contextPtr);
    if (res == NULL) {
        if (virUUIDGenerate(uuid)) {
            lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                     _("failed to generate uuid"));
            return(-1);
        }
    } else {
        if (virUUIDParse(res, uuid) < 0) {
            lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                     _("invalid uuid element"));
            VIR_FREE(res);
            return(-1);
        }
        VIR_FREE(res);
    }
    return(0);
}

static int lxcParseDomainMounts(virConnectPtr conn,
                                lxc_mount_t **mounts,
                                xmlXPathContextPtr contextPtr)
{
    int rc = -1;
    int i;
    lxc_mount_t *mountObj;
    lxc_mount_t *prevObj = NULL;
    int nmounts = 0;
    xmlNodePtr *list;
    int res;

    res = virXPathNodeSet(conn, "/domain/devices/filesystem", contextPtr, &list);
    if (res > 0) {
        for (i = 0; i < res; ++i) {
            if (VIR_ALLOC(mountObj) < 0) {
                lxcError(conn, NULL, VIR_ERR_NO_MEMORY, "mount");
                goto parse_complete;
            }

            rc = lxcParseMountXML(conn, list[i], mountObj);
            if (0 > rc) {
                VIR_FREE(mountObj);
                goto parse_complete;
            }

            /* set the linked list pointers */
            nmounts++;
            mountObj->next = NULL;
            if (0 == i) {
                *mounts = mountObj;
            } else {
                prevObj->next = mountObj;
            }
            prevObj = mountObj;
        }
        VIR_FREE(list);
    }

    rc = nmounts;

parse_complete:
    return rc;
}

static int lxcParseDomainInit(virConnectPtr conn, char** init,
                              xmlXPathContextPtr contextPtr)
{
    char *res;

    res = virXPathString(conn, "string(/domain/os/init[1])", contextPtr);
    if (res == NULL) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("invalid or missing init element"));
        return(-1);
    }

    if (strlen(res) >= PATH_MAX - 1) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("init string too long"));
        VIR_FREE(res);
        return(-1);
    }

    *init = res;

    return(0);
}


static int lxcParseDomainTty(virConnectPtr conn, char **tty, xmlXPathContextPtr contextPtr)
{
    char *res;

    res = virXPathString(conn, "string(/domain/devices/console[1]/@tty)", contextPtr);
    if (res == NULL) {
        /* make sure the tty string is empty */
        *tty = strdup("");
        if (*tty == NULL) {
            lxcError(conn, NULL, VIR_ERR_NO_MEMORY, NULL);
            return(-1);
        }
    } else {
        *tty = res;
    }

    return(0);
}

static int lxcParseDomainMemory(virConnectPtr conn, int* memory, xmlXPathContextPtr contextPtr)
{
    long res;
    int rc;

    rc = virXPathLong(conn, "string(/domain/memory[1])", contextPtr, &res);
    if ((rc == -2) || ((rc == 0) && (res <= 0))) {
        *memory = -1;
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("invalid memory value"));
    } else if (rc < 0) {
        /* not an error, default to an invalid value so it's not used */
        *memory = -1;
    } else {
        *memory = (int) res;
    }
    return(0);
}

static lxc_vm_def_t * lxcParseXML(virConnectPtr conn, xmlDocPtr docPtr)
{
    xmlNodePtr rootNodePtr = NULL;
    xmlXPathContextPtr contextPtr = NULL;
    xmlChar *xmlProp = NULL;
    lxc_vm_def_t *containerDef;

    if (VIR_ALLOC(containerDef) < 0) {
        lxcError(conn, NULL, VIR_ERR_NO_MEMORY, "containerDef");
        return NULL;
    }

    /* Prepare parser / xpath context */
    rootNodePtr = xmlDocGetRootElement(docPtr);
    if ((rootNodePtr == NULL) ||
       (!xmlStrEqual(rootNodePtr->name, BAD_CAST "domain"))) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("invalid root element"));
        goto error;
    }

    contextPtr = xmlXPathNewContext(docPtr);
    if (contextPtr == NULL) {
        lxcError(conn, NULL, VIR_ERR_NO_MEMORY, "context");
        goto error;
    }

    /* Verify the domain type is linuxcontainer */
    if (!(xmlProp = xmlGetProp(rootNodePtr, BAD_CAST "type"))) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("missing domain type"));
        goto error;
    }

    if (!(xmlStrEqual(xmlProp, BAD_CAST LXC_DOMAIN_TYPE))) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("invalid domain type"));
        goto error;
    }
    VIR_FREE(xmlProp);

    if ((xmlProp = xmlGetProp(rootNodePtr, BAD_CAST "id"))) {
        if (0 > virStrToLong_i((char*)xmlProp, NULL, 10, &(containerDef->id))) {
            lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                     _("invalid domain id"));
            goto error;
        }

        /* verify the container process still exists */
        if (1 != lxcCheckContainerProcess(containerDef)) {
            containerDef->id = -1;
        }

    } else {
        containerDef->id = -1;
    }
    VIR_FREE(xmlProp);

    if (lxcParseDomainName(conn, &(containerDef->name), contextPtr) < 0) {
        goto error;
    }

    if (lxcParseDomainInit(conn, &(containerDef->init), contextPtr) < 0) {
        goto error;
    }

    if (lxcParseDomainUUID(conn, containerDef->uuid, contextPtr) < 0) {
        goto error;
    }

    containerDef->nmounts = lxcParseDomainMounts(conn, &(containerDef->mounts),
                                                 contextPtr);
    if (0 > containerDef->nmounts) {
        goto error;
    }

    containerDef->numNets = lxcParseDomainInterfaces(conn,
                                                     &(containerDef->nets),
                                                     contextPtr);
    if (0 > containerDef->numNets) {
        goto error;
    }

    if (lxcParseDomainTty(conn, &(containerDef->tty), contextPtr) < 0) {
        goto error;
    }

    if (lxcParseDomainMemory(conn, &(containerDef->maxMemory), contextPtr) < 0) {
        goto error;
    }

    xmlXPathFreeContext(contextPtr);

    return containerDef;

 error:
    VIR_FREE(xmlProp);
    xmlXPathFreeContext(contextPtr);
    lxcFreeVMDef(containerDef);

    return NULL;
}


lxc_vm_def_t * lxcParseVMDef(virConnectPtr conn,
                             const char* xmlString,
                             const char* fileName)
{
    xmlDocPtr xml;
    lxc_vm_def_t *containerDef;

    xml = xmlReadDoc(BAD_CAST xmlString,
                     fileName ? fileName : "domain.xml",
                     NULL, XML_PARSE_NOENT |
                     XML_PARSE_NONET | XML_PARSE_NOERROR |
                     XML_PARSE_NOWARNING);
    if (!xml) {
        lxcError(conn, NULL, VIR_ERR_XML_ERROR, NULL);
        return NULL;
    }

    containerDef = lxcParseXML(conn, xml);

    xmlFreeDoc(xml);

    return containerDef;
}

lxc_vm_t * lxcAssignVMDef(virConnectPtr conn,
                          lxc_driver_t *driver,
                          lxc_vm_def_t *def)
{
    lxc_vm_t *vm = NULL;

    if ((vm = lxcFindVMByName(driver, def->name))) {
        if (!lxcIsActiveVM(vm)) {
            lxcFreeVMDef(vm->def);
            vm->def = def;
        } else {
            lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                     _("Can't redefine active VM with name %s"), def->name);
            return NULL;
        }

        return vm;
    }

    if (VIR_ALLOC(vm) < 0) {
        lxcError(conn, NULL, VIR_ERR_NO_MEMORY, "vm");
        return NULL;
    }

    vm->pid = -1;
    vm->def = def;
    vm->next = driver->vms;

    driver->vms = vm;

    if (lxcIsActiveVM(vm)) {
        vm->state = VIR_DOMAIN_RUNNING;
        driver->nactivevms++;
    } else {
        vm->state = VIR_DOMAIN_SHUTOFF;
        driver->ninactivevms++;
    }

    return vm;
}

/**
 * lxcCheckContainerProcess:
 * @def: Ptr to VM definition
 *
 * Checks if the container process (stored at def->id is running
 *
 * Returns on success or -1 in case of error
 * 0  - no process with id vm->def->id
 * 1  - container process exists
 * -1 - error
 */
int lxcCheckContainerProcess(lxc_vm_def_t *def)
{
    int rc = -1;

    if (1 < def->id) {
        if (-1 == kill(def->id, 0)) {
            if (ESRCH == errno) {
                rc = 0;
                DEBUG("pid %d no longer exists", def->id);
                goto done;
            }

            lxcError(NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                     _("error checking container process: %d %s"),
                     def->id, strerror(errno));
            goto done;
        }

        DEBUG("pid %d still exists", def->id);
        rc = 1;
        goto done;
    }

    rc = 0;

done:
    return rc;
}

void lxcRemoveInactiveVM(lxc_driver_t *driver,
                         lxc_vm_t *vm)
{
    lxc_vm_t *prevVm = NULL;
    lxc_vm_t *curVm;

    for (curVm = driver->vms;
         (curVm != vm) && (NULL != curVm);
         curVm = curVm->next) {
        prevVm = curVm;
    }

    if (curVm) {
        if (prevVm) {
            prevVm->next = curVm->next;
        } else {
            driver->vms = curVm->next;
        }

        driver->ninactivevms--;
    }

    lxcFreeVM(vm);
}

/* Save a container's config data into a persistent file */
int lxcSaveConfig(virConnectPtr conn,
                  lxc_driver_t *driver,
                  lxc_vm_t *vm,
                  lxc_vm_def_t *def)
{
    int rc = -1;
    char *xmlDef;
    int fd = -1;
    int amtToWrite;

    if (!(xmlDef = lxcGenerateXML(conn, driver, vm, def))) {
        return -1;
    }

    if ((fd = open(vm->configFile,
                  O_WRONLY | O_CREAT | O_TRUNC,
                  S_IRUSR | S_IWUSR )) < 0) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("cannot create config file %s: %s"),
                  vm->configFile, strerror(errno));
        goto cleanup;
    }

    amtToWrite = strlen(xmlDef);
    if (safewrite(fd, xmlDef, amtToWrite) < 0) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("cannot write config file %s: %s"),
                 vm->configFile, strerror(errno));
        goto cleanup;
    }

    if (close(fd) < 0) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("cannot save config file %s: %s"),
                 vm->configFile, strerror(errno));
        goto cleanup;
    }

    rc = 0;

 cleanup:
    if (fd != -1) {
        close(fd);
    }

    VIR_FREE(xmlDef);

    return rc;
}

int lxcSaveVMDef(virConnectPtr conn,
                 lxc_driver_t *driver,
                 lxc_vm_t *vm,
                 lxc_vm_def_t *def)
{
    int rc = -1;

    if (vm->configFile[0] == '\0') {
        if ((rc = virFileMakePath(driver->configDir))) {
            lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                     _("cannot create config directory %s: %s"),
                     driver->configDir, strerror(rc));
            goto save_complete;
        }

        if (virFileBuildPath(driver->configDir, vm->def->name, ".xml",
                            vm->configFile, PATH_MAX) < 0) {
            lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                     _("cannot construct config file path"));
            goto save_complete;
        }

        strncpy(vm->configFileBase, vm->def->name, PATH_MAX-1);
        strncat(vm->configFileBase, ".xml", PATH_MAX - strlen(vm->def->name)-1);

    }

    rc = lxcSaveConfig(conn, driver, vm, def);

save_complete:
    return rc;
}



static lxc_vm_t * lxcLoadConfig(lxc_driver_t *driver,
                                const char *file,
                                const char *fullFilePath,
                                const char *xmlData)
{
    lxc_vm_def_t *containerDef;
    lxc_vm_t * vm;

    containerDef = lxcParseVMDef(NULL, xmlData, file);
    if (NULL == containerDef) {
        DEBUG0("Error parsing container config");
        return NULL;
    }

    if (!virFileMatchesNameSuffix(file, containerDef->name, ".xml")) {
        DEBUG0("Container name does not match config file name");
        lxcFreeVMDef(containerDef);
        return NULL;
    }

    vm = lxcAssignVMDef(NULL, driver, containerDef);
    if (NULL == vm) {
        DEBUG0("Failed to load container config");
        lxcFreeVMDef(containerDef);
        return NULL;
    }

    strncpy(vm->configFile, fullFilePath, PATH_MAX);
    vm->configFile[PATH_MAX-1] = '\0';

    strncpy(vm->configFileBase, file, PATH_MAX);
    vm->configFile[PATH_MAX-1] = '\0';

    return vm;
}

int lxcLoadDriverConfig(lxc_driver_t *driver)
{
    /* Set the container configuration directory */
    if ((driver->configDir = strdup(SYSCONF_DIR "/libvirt/lxc")) == NULL)
        goto no_memory;
    if ((driver->stateDir = strdup(LOCAL_STATE_DIR "/run/libvirt/lxc")) == NULL)
        goto no_memory;
    if ((driver->logDir = strdup(LOCAL_STATE_DIR "/log/libvirt/lxc")) == NULL)
        goto no_memory;

    return 0;

no_memory:
    lxcError(NULL, NULL, VIR_ERR_NO_MEMORY, "configDir");
    return -1;
}

int lxcLoadContainerConfigFile(lxc_driver_t *driver,
                               const char *file)
{
    int rc = -1;
    char tempPath[PATH_MAX];
    char* xmlData;

    rc = virFileBuildPath(driver->configDir, file, NULL, tempPath,
                          PATH_MAX);
    if (0 > rc) {
        DEBUG0("config file name too long");
        goto load_complete;
    }

    if ((rc = virFileReadAll(tempPath, LXC_MAX_XML_LENGTH, &xmlData)) < 0) {
        goto load_complete;
    }

    lxcLoadConfig(driver, file, tempPath, xmlData);

    VIR_FREE(xmlData);

load_complete:
    return rc;
}

int lxcLoadContainerInfo(lxc_driver_t *driver)
{
    int rc = -1;
    DIR *dir;
    struct dirent *dirEntry;

    if (!(dir = opendir(driver->configDir))) {
        if (ENOENT == errno) {
            /* no config dir => no containers */
            rc = 0;
        } else {
            lxcError(NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                     _("failed to open config directory %s: %s"),
                     driver->configDir, strerror(errno));
        }

        goto load_complete;
    }

    while ((dirEntry = readdir(dir))) {
        if (dirEntry->d_name[0] == '.') {
            continue;
        }

        if (!virFileHasSuffix(dirEntry->d_name, ".xml")) {
            continue;
        }

        lxcLoadContainerConfigFile(driver, dirEntry->d_name);
    }

    closedir(dir);

    rc = 0;

load_complete:
    return rc;
}

/* Generate an XML document describing the vm's configuration */
char *lxcGenerateXML(virConnectPtr conn,
                     lxc_driver_t *driver ATTRIBUTE_UNUSED,
                     lxc_vm_t *vm,
                     lxc_vm_def_t *def)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    unsigned char *uuid;
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    lxc_mount_t *mount;
    lxc_net_def_t *net;

    if (lxcIsActiveVM(vm))
        virBufferVSprintf(&buf, "<domain type='%s' id='%d'>\n",
                          LXC_DOMAIN_TYPE, vm->def->id);
    else
        virBufferVSprintf(&buf, "<domain type='%s'>\n",
                          LXC_DOMAIN_TYPE);

    virBufferVSprintf(&buf, "    <name>%s</name>\n", def->name);

    uuid = def->uuid;
    virUUIDFormat(uuid, uuidstr);
    virBufferVSprintf(&buf, "    <uuid>%s</uuid>\n", uuidstr);
    virBufferAddLit(&buf, "    <os>\n");
    virBufferVSprintf(&buf, "        <init>%s</init>\n", def->init);
    virBufferAddLit(&buf, "    </os>\n");
    virBufferVSprintf(&buf, "    <memory>%d</memory>\n", def->maxMemory);
    virBufferAddLit(&buf, "    <devices>\n");

    /* loop adding mounts */
    for (mount = def->mounts; mount; mount = mount->next) {
        virBufferAddLit(&buf, "        <filesystem type='mount'>\n");
        virBufferVSprintf(&buf, "            <source dir='%s'/>\n",
                          mount->source);
        virBufferVSprintf(&buf, "            <target dir='%s'/>\n",
                          mount->target);
        virBufferAddLit(&buf, "        </filesystem>\n");
    }

    /* loop adding nets */
    for (net = def->nets; net; net = net->next) {
        if (net->type == LXC_NET_NETWORK) {
            virBufferAddLit(&buf, "        <interface type='network'>\n");
            virBufferVSprintf(&buf, "            <source network='%s'/>\n",
                              net->txName);
        } else {
            virBufferAddLit(&buf, "        <interface type='bridge'>\n");
            virBufferVSprintf(&buf, "            <source bridge='%s'/>\n",
                              net->txName);
        }

        if (NULL != net->parentVeth) {
            virBufferVSprintf(&buf, "            <target dev='%s'/>\n",
                              net->parentVeth);
        }

        virBufferAddLit(&buf, "        </interface>\n");

    }

    virBufferVSprintf(&buf, "        <console tty='%s'/>\n", def->tty);
    virBufferAddLit(&buf, "    </devices>\n");
    virBufferAddLit(&buf, "</domain>\n");

    if (virBufferError(&buf)) {
        lxcError(conn, NULL, VIR_ERR_NO_MEMORY,_("allocate buffer"));
        return NULL;
    }

    return virBufferContentAndReset(&buf);
}

void lxcFreeVMDef(lxc_vm_def_t *vmdef)
{
    lxc_mount_t *curMount;
    lxc_mount_t *nextMount;
    lxc_net_def_t *curNet;
    lxc_net_def_t *nextNet;

    if (vmdef == NULL)
        return;

    curMount = vmdef->mounts;
    while (curMount) {
        nextMount = curMount->next;
        VIR_FREE(curMount);
        curMount = nextMount;
    }

    curNet = vmdef->nets;
    while (curNet) {
        nextNet = curNet->next;
        VIR_FREE(curNet->parentVeth);
        VIR_FREE(curNet->txName);
        VIR_FREE(curNet);
        curNet = nextNet;
    }

    VIR_FREE(vmdef->name);
    VIR_FREE(vmdef->init);
    VIR_FREE(vmdef->tty);
    VIR_FREE(vmdef);
}

void lxcFreeVMs(lxc_vm_t *vms)
{
    lxc_vm_t *curVm = vms;
    lxc_vm_t *nextVm;

    while (curVm) {
        nextVm = curVm->next;
        lxcFreeVM(curVm);
        curVm = nextVm;
    }
}

void lxcFreeVM(lxc_vm_t *vm)
{
    lxcFreeVMDef(vm->def);
    VIR_FREE(vm);
}

lxc_vm_t *lxcFindVMByID(const lxc_driver_t *driver, int id)
{
    lxc_vm_t *vm;

    for (vm = driver->vms; vm; vm = vm->next) {
        if (lxcIsActiveVM(vm) && (vm->def->id == id)) {
            return vm;
        }

    }

    return NULL;
}

lxc_vm_t *lxcFindVMByUUID(const lxc_driver_t *driver,
                          const unsigned char *uuid)
{
    lxc_vm_t *vm;

    for (vm = driver->vms; vm; vm = vm->next) {
        if (!memcmp(vm->def->uuid, uuid, VIR_UUID_BUFLEN)) {
            return vm;
        }
    }

    return NULL;
}

lxc_vm_t *lxcFindVMByName(const lxc_driver_t *driver,
                          const char *name)
{
    lxc_vm_t *vm;

    for (vm = driver->vms; vm; vm = vm->next) {
        if (STREQ(vm->def->name, name)) {
            return vm;
        }
    }

    return NULL;
}

int lxcDeleteConfig(virConnectPtr conn,
                    lxc_driver_t *driver ATTRIBUTE_UNUSED,
                    const char *configFile,
                    const char *name)
{
    if (!configFile[0]) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("no config file for %s"), name);
        return -1;
    }

    if (unlink(configFile) < 0) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("cannot remove config for %s"), name);
        return -1;
    }

    return 0;
}

#endif /* WITH_LXC */
