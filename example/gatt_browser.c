/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

// *****************************************************************************
/* EXAMPLE_START(gatt_browser): GATT Client - Discovering primary services and their characteristics
 * 
 * @text This example shows how to use the GATT Client
 * API to discover primary services and their characteristics of the first found
 * device that is advertising its services.
 *
 * The logic is divided between the HCI and GATT client packet handlers.
 * The HCI packet handler is responsible for finding a remote device, 
 * connecting to it, and for starting the first GATT client query.
 * Then, the GATT client packet handler receives all primary services and
 * requests the characteristics of the last one to keep the example short.
 *
 */
// *****************************************************************************

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack.h"

typedef struct advertising_report {
    uint8_t   type;
    uint8_t   event_type;
    uint8_t   address_type;
    bd_addr_t address;
    uint8_t   rssi;
    uint8_t   length;
    const uint8_t * data;
} advertising_report_t;

static bd_addr_t cmdline_addr = { };
static int cmdline_addr_found = 0;

static hci_con_handle_t connection_handler;
static gatt_client_service_t services[40];
static int service_count = 0;
static int service_index = 0;

static btstack_packet_callback_registration_t hci_event_callback_registration;

/* @section GATT client setup
 *
 * @text In the setup phase, a GATT client must register the HCI and GATT client
 * packet handlers, as shown in Listing GATTClientSetup.
 * Additionally, the security manager can be setup, if signed writes, or
 * encrypted, or authenticated connection are required, to access the
 * characteristics, as explained in Section on [SMP](../protocols/#sec:smpProtocols).
 */

/* LISTING_START(GATTClientSetup): Setting up GATT client */

// Handles connect, disconnect, and advertising report events,  
// starts the GATT client, and sends the first query.
static void handle_hci_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// Handles GATT client query results, sends queries and the 
// GAP disconnect command when the querying is done.
static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void gatt_client_setup(void){

    // register for HCI events
    hci_event_callback_registration.callback = &handle_hci_event;
    hci_add_event_handler(&hci_event_callback_registration);

    // Initialize L2CAP and register HCI event handler
    l2cap_init();

    // Initialize GATT client 
    gatt_client_init();

    // Optinoally, Setup security manager
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
}
/* LISTING_END */

static void printUUID(uint8_t * uuid128, uint16_t uuid16){
    if (uuid16){
        printf("%04x",uuid16);
    } else {
        printf("%s", uuid128_to_str(uuid128));
    }
}

static void dump_advertising_report(advertising_report_t * e){
    printf("    * adv. event: evt-type %u, addr-type %u, addr %s, rssi %u, length adv %u, data: ", e->event_type,
           e->address_type, bd_addr_to_str(e->address), e->rssi, e->length);
    printf_hexdump(e->data, e->length);
    
}

static void dump_characteristic(gatt_client_characteristic_t * characteristic){
    printf("    * characteristic: [0x%04x-0x%04x-0x%04x], properties 0x%02x, uuid ",
                            characteristic->start_handle, characteristic->value_handle, characteristic->end_handle, characteristic->properties);
    printUUID(characteristic->uuid128, characteristic->uuid16);
    printf("\n");
}

static void dump_service(gatt_client_service_t * service){
    printf("    * service: [0x%04x-0x%04x], uuid ", service->start_group_handle, service->end_group_handle);
    printUUID(service->uuid128, service->uuid16);
    printf("\n");
}

static void fill_advertising_report_from_packet(advertising_report_t * report, uint8_t *packet){
    gap_event_advertising_report_get_address(packet, report->address);
    report->event_type = gap_event_advertising_report_get_advertising_event_type(packet);
    report->address_type = gap_event_advertising_report_get_address_type(packet);
    report->rssi = gap_event_advertising_report_get_rssi(packet);
    report->length = gap_event_advertising_report_get_data_length(packet);
    report->data = gap_event_advertising_report_get_data(packet);
}

/* @section HCI packet handler
 * 
 * @text The HCI packet handler has to start the scanning, 
 * to find the first advertising device, to stop scanning, to connect
 * to and later to disconnect from it, to start the GATT client upon
 * the connection is completed, and to send the first query - in this
 * case the gatt_client_discover_primary_services() is called, see 
 * Listing GATTBrowserHCIPacketHandler.  
 */

/* LISTING_START(GATTBrowserHCIPacketHandler): Connecting and disconnecting from the GATT client */
static void handle_hci_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    if (packet_type != HCI_EVENT_PACKET) return;
    advertising_report_t report;
    
    uint8_t event = hci_event_packet_get_type(packet);
    switch (event) {
        case BTSTACK_EVENT_STATE:
            // BTstack activated, get started
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
            if (cmdline_addr_found){
                printf("Trying to connect to %s\n", bd_addr_to_str(cmdline_addr));
                gap_connect(cmdline_addr, 0);
                break;
            }
            printf("BTstack activated, start scanning!\n");
            gap_set_scan_parameters(0,0x0030, 0x0030);
            gap_start_scan();
            break;
        case GAP_EVENT_ADVERTISING_REPORT:
            fill_advertising_report_from_packet(&report, packet);
            dump_advertising_report(&report);

            // stop scanning, and connect to the device
            gap_stop_scan();
            gap_connect(report.address,report.address_type);
            break;
        case HCI_EVENT_LE_META:
            // wait for connection complete
            if (packet[2] !=  HCI_SUBEVENT_LE_CONNECTION_COMPLETE) break;
            connection_handler = hci_subevent_le_connection_complete_get_connection_handle(packet);
            // query primary services
            gatt_client_discover_primary_services(handle_gatt_client_event, connection_handler);
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            printf("\nGATT browser - DISCONNECTED\n");
            exit(0);
            break;
        default:
            break;
    }
}
/* LISTING_END */

/* @section GATT Client event handler
 *
 * @text Query results and further queries are handled by the GATT client packet
 * handler, as shown in Listing GATTBrowserQueryHandler. Here, upon
 * receiving the primary services, the
 * gatt_client_discover_characteristics_for_service() query for the last
 * received service is sent. After receiving the characteristics for the service,
 * gap_disconnect is called to terminate the connection. Upon
 * disconnect, the HCI packet handler receives the disconnect complete event.  
 */

/* LISTING_START(GATTBrowserQueryHandler): Handling of the GATT client queries */
static int search_services = 1;

static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    gatt_client_service_t service;
    gatt_client_characteristic_t characteristic;
    switch(hci_event_packet_get_type(packet)){
        case GATT_EVENT_SERVICE_QUERY_RESULT:\
            gatt_event_service_query_result_get_service(packet, &service);
            dump_service(&service);
            services[service_count++] = service;
            break;
        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
            gatt_event_characteristic_query_result_get_characteristic(packet, &characteristic);
            dump_characteristic(&characteristic);
            break;
        case GATT_EVENT_QUERY_COMPLETE:
            if (search_services){
                // GATT_EVENT_QUERY_COMPLETE of search services 
                service_index = 0;
                printf("\nGATT browser - CHARACTERISTIC for SERVICE %s\n", uuid128_to_str(service.uuid128));
                search_services = 0;
                gatt_client_discover_characteristics_for_service(handle_gatt_client_event, connection_handler, &services[service_index]);
            } else {
                // GATT_EVENT_QUERY_COMPLETE of search characteristics
                if (service_index < service_count) {
                    service = services[service_index++];
                    printf("\nGATT browser - CHARACTERISTIC for SERVICE %s, [0x%04x-0x%04x]\n",
                        uuid128_to_str(service.uuid128), service.start_group_handle, service.end_group_handle);
                    gatt_client_discover_characteristics_for_service(handle_gatt_client_event, connection_handler, &service);
                    break;
                }
                service_index = 0;
                gap_disconnect(connection_handler); 
            }
            break;
        default:
            break;
    }
}
/* LISTING_END */

static void usage(const char *name){
	fprintf(stderr, "\nUsage: %s [-a|--address aa:bb:cc:dd:ee:ff]\n", name);
	fprintf(stderr, "If no argument is provided, GATT browser will start scanning and connect to the first found device.\nTo connect to a specific device use argument [-a].\n\n");
}

int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){

    int arg = 1;
    cmdline_addr_found = 0;
    
    while (arg < argc) {
		if(!strcmp(argv[arg], "-a") || !strcmp(argv[arg], "--address")){
			arg++;
			cmdline_addr_found = sscanf_bd_addr(argv[arg], cmdline_addr);
            arg++;
            continue;
        }
        usage(argv[0]);
        return 0;
	}

    gatt_client_setup();

    // turn on!
    hci_power_control(HCI_POWER_ON);
    
    return 0;
}

/* EXAMPLE_END */


