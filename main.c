/***********************************************************************************************//**
 * \file   main.c
 * \brief  BT Mesh provisioner example
 *
 *  Simple provisioner example that can be dropped on top of the soc-btmesh-light example, by replacing
 *  the main.c with this file.
 *
 *  Additional changes needed:
 *  - Configuration Client model needs to be added into the DCD
 *  - adjust following parameters in the memory configuration (in the DCD editor)
 *     - Max Provisioned Devices,
 *     - Max Provisioned Device Netkeys
 *     - Max Foundation Client Cmds
 *     Default value for these is zero. Must use non-zero values to enable provisioning and configuration
 *     of devices.
 *  - After modifying the DCD and the memory config, remember to press Generate button to re-generate the dcd.c source
 *
 *  Known issues and limitations:
 *   - this is an inital provisioner example code with limited testing and features
 *   - code cleanup and better error handling TBD.
 *   - configuration of vendor models TBD.
 *
 *
 ***************************************************************************************************
 * <b> (C) Copyright 2017 Silicon Labs, http://www.silabs.com</b>
 ***************************************************************************************************
 * This file is licensed under the Silabs License Agreement. See the file
 * "Silabs_License_Agreement.txt" for details. Before using this software for
 * any purpose, you must agree to the terms of that agreement.
 **************************************************************************************************/

/* C Standard Library headers */
#include <stdlib.h>
#include <stdio.h>

/* Board headers */
#include "init_mcu.h"
#include "init_board.h"
#include "init_app.h"
#include "ble-configuration.h"
#include "board_features.h"

/* WSTK specific includes */
#include "retargetserial.h"

/* Bluetooth stack headers */
#include "bg_types.h"
#include "native_gecko.h"
#include "gatt_db.h"
#include <gecko_configuration.h>
#include "mesh_generic_model_capi_types.h"
#include "mesh_lighting_model_capi_types.h"
#include "mesh_lib.h"
#include <mesh_sizes.h>

/* Libraries containing default Gecko configuration values */
#include "em_emu.h"
#include "em_cmu.h"
#include <em_gpio.h>

/* Device initialization header */
#include "hal-config.h"

#if defined(HAL_CONFIG)
#include "bsphalconfig.h"
#else
#include "bspconfig.h"
#endif

/***********************************************************************************************//**
 * @addtogroup Application
 * @{
 **************************************************************************************************/

uint8_t netkey_id = 0xff;
uint8_t appkey_id = 0xff;
uint8_t ask_user_input = false;
uint16 provisionee_address = 0xFFFF;

uint8_t uuid_copy_buf[16];

/***********************************************************************************************//**
 * @addtogroup app
 * @{
 **************************************************************************************************/

struct mesh_generic_state current, target;

void mesh_native_bgapi_init(void);
bool mesh_bgapi_listener(struct gecko_cmd_packet *evt);

// Maximum number of simultaneous Bluetooth connections
#define MAX_CONNECTIONS 2

// heap for Bluetooth stack
uint8_t bluetooth_stack_heap[DEFAULT_BLUETOOTH_HEAP(MAX_CONNECTIONS) + BTMESH_HEAP_SIZE + 1760];

typedef struct {
	uint16 err;
	const char *pShortDescription;
} tsErrCode;

#define STATUS_OK                      0
#define STATUS_BUSY                    0x181

/*
 * Look-up table for mapping error codes to strings. Not a complete
 * list, for full description of error codes, see
 * Bluetooth LE and Mesh Software API Reference Manual */

tsErrCode _sErrCodes[] = {
		{
				0x0c01,
				"already_exists" },
		{
				0x0c02,
				"does_not_exist" },
		{
				0x0c03,
				"limit_reached" },
		{
				0x0c04,
				"invalid_address" },
		{
				0x0c05,
				"malformed_data" }, };

const char err_unknown[] = "<?>";

const char * res2str(uint16 err) {
	int i;

	for (i = 0; i < sizeof(_sErrCodes) / sizeof(tsErrCode); i++) {
		if (err == _sErrCodes[i].err) {
			return _sErrCodes[i].pShortDescription;
		}
	}

	// code was not found in the lookup table
	return err_unknown;
}

/*
 * Maximum number of Bluetooth advertisement sets.
 * 1 is allocated for Bluetooth LE stack
 * 1 one for Bluetooth mesh stack
 * 1 needs to be allocated for each Bluetooth mesh network
 *   - Currently up to 4 networks are supported at a time
 */
#define MAX_ADVERTISERS (2 + 4)

// Bluetooth stack configuration
const gecko_configuration_t config = {
		.bluetooth.max_connections = MAX_CONNECTIONS,
		.bluetooth.max_advertisers = MAX_ADVERTISERS,
		.bluetooth.heap = bluetooth_stack_heap,
		.bluetooth.heap_size = sizeof(bluetooth_stack_heap) - BTMESH_HEAP_SIZE,
		.bluetooth.sleep_clock_accuracy = 100,
		.gattdb = &bg_gattdb_data,
		.btmesh_heap_size = BTMESH_HEAP_SIZE,
#if (HAL_PA_ENABLE) && defined(FEATURE_PA_HIGH_POWER)
		.pa.config_enable = 1, // Enable high power PA
		.pa.input = GECKO_RADIO_PA_INPUT_VBAT,// Configure PA input to VBAT
#endif // (HAL_PA_ENABLE) && defined(FEATURE_PA_HIGH_POWER)
	};

/** Timer Frequency used. */
#define TIMER_CLK_FREQ ((uint32)32768)
/** Convert msec to timer ticks. */
#define TIMER_MS_2_TIMERTICK(ms) ((TIMER_CLK_FREQ * ms) / 1000)

#define TIMER_ID_RESTART    78
#define TIMER_ID_FACTORY_RESET  77
#define TMIER_ID_BUTTON_POLL              49

#define TIMER_ID_GET_DCD				  20
#define TIMER_ID_APPKEY_ADD				  21
#define TIMER_ID_APPKEY_BIND		      22
#define TIMER_ID_PUB_SET 				  23
#define TIMER_ID_SUB_ADD				  24


/** global variables */
static uint8 num_connections = 0; /* number of active Bluetooth connections */
static uint8 conn_handle = 0xFF; /* handle of the last opened LE connection */

enum {
	init,
	scanning,
	provisioning,
	provisioned,
	waiting_dcd,
	waiting_appkey_ack,
	waiting_bind_ack,
	waiting_pub_ack,
	waiting_sub_ack
} state;

static void handle_gecko_event(uint32_t evt_id, struct gecko_cmd_packet *evt);

/**
 * button initialization. Configure pushbuttons PB0,PB1
 * as inputs.
 */
static void button_init() {
	// configure pushbutton PB0 and PB1 as inputs, with pull-up enabled
	GPIO_PinModeSet(BSP_BUTTON0_PORT, BSP_BUTTON0_PIN, gpioModeInputPull, 1);
	GPIO_PinModeSet(BSP_BUTTON1_PORT, BSP_BUTTON1_PIN, gpioModeInputPull, 1);
}

/**
 * Set device name in the GATT database. A unique name is generated using
 * the two last bytes from the Bluetooth address of this device. Name is also
 * displayed on the LCD.
 */
void set_device_name(bd_addr *pAddr) {
	char name[20];
	uint16 res;

	// create unique device name using the last two bytes of the Bluetooth address
	sprintf(name, "light node %x:%x", pAddr->addr[1], pAddr->addr[0]);

	printf("Device name: '%s'\r\n", name);

	res = gecko_cmd_gatt_server_write_attribute_value(gattdb_device_name, 0, strlen(name), (uint8 *) name)->result;
	if (res) {
		printf("gecko_cmd_gatt_server_write_attribute_value() failed, code %x\r\n", res);
	}
}

/**
 *  this function is called to initiate factory reset. Factory reset may be initiated
 *  by keeping one of the WSTK pushbuttons pressed during reboot. Factory reset is also
 *  performed if it is requested by the provisioner (event gecko_evt_mesh_node_reset_id)
 */
void initiate_factory_reset(void) {
	printf("factory reset\r\n");

	/* if connection is open then close it before rebooting */
	if (conn_handle != 0xFF) {
		gecko_cmd_le_connection_close(conn_handle);
	}

	/* perform a factory reset by erasing PS storage. This removes all the keys and other settings
	 that have been configured for this node */
	gecko_cmd_flash_ps_erase_all();
	// reboot after a small delay
	gecko_cmd_hardware_set_soft_timer(2 * 32768, TIMER_ID_FACTORY_RESET, 1);
}

int main() {
	// Initialize device
	initMcu();
	// Initialize board
	initBoard();
	// Initialize application
	initApp();

	gecko_stack_init(&config);
	gecko_bgapi_class_dfu_init();
	gecko_bgapi_class_system_init();
	gecko_bgapi_class_le_gap_init();
	gecko_bgapi_class_le_connection_init();
	gecko_bgapi_class_gatt_init();
	gecko_bgapi_class_gatt_server_init();
	gecko_bgapi_class_endpoint_init();
	gecko_bgapi_class_hardware_init();
	gecko_bgapi_class_flash_init();
	gecko_bgapi_class_test_init();
	gecko_bgapi_class_sm_init();
	gecko_bgapi_class_mesh_prov_init();
	gecko_bgapi_class_mesh_proxy_init();
	gecko_bgapi_class_mesh_proxy_client_init();
	gecko_bgapi_class_mesh_proxy_server_init();
	mesh_native_bgapi_init();
	gecko_initCoexHAL();

	RETARGET_SerialInit();

	/* initialize LEDs and buttons. Note: some radio boards share the same GPIO for button & LED.
	 * Initialization is done in this order so that default configuration will be "button" for those
	 * radio boards with shared pins. LEDS_init() is called later as needed to (re)initialize the LEDs
	 * */
	button_init();

	while (1) {
		struct gecko_cmd_packet *evt = gecko_wait_event();
		bool pass = mesh_bgapi_listener(evt);
		if (pass) {
			handle_gecko_event(BGLIB_MSG_ID(evt->header), evt);
		}
	}
}

static void button_poll() {

	if (ask_user_input == false) {
		return;
	}

	if (GPIO_PinInGet(BSP_BUTTON1_PORT, BSP_BUTTON1_PIN) == 0) {
		ask_user_input = false;
		printf("Sending prov request\r\n");

		struct gecko_msg_mesh_prov_provision_device_rsp_t *prov_resp_adv;
		prov_resp_adv = gecko_cmd_mesh_prov_provision_device(netkey_id, 16, uuid_copy_buf);

		if (prov_resp_adv->result == 0) {
			printf("Successful call of gecko_cmd_mesh_prov_provision_device\r\n");
			state = provisioning;
		} else {
			printf("Failed call to provision node. %x\r\n", prov_resp_adv->result);
		}
	} else if (GPIO_PinInGet(BSP_BUTTON0_PORT, BSP_BUTTON0_PIN) == 0) {
		ask_user_input = false;
	}

}

typedef struct {
	uint8 numElem;
	uint8 numModels;

	// reserve space for up to 8 SIG models
	uint16 SIG_models[8];
	uint8 numSIGModels;

	uint16_t vendor_models[4];
	uint8_t numVendorModels;
} tsDCD;

// DCD of the last provisioned device:
tsDCD _sDCD;

static void DCD_decode(struct gecko_msg_mesh_prov_dcd_status_evt_t *pDCD) {
	uint8 *pu8;
	uint16 *pu16;
	int i;

	printf("DCD: company ID %4.4x, Product ID %4.4x\r\n", pDCD->cid, pDCD->pid);

	_sDCD.numElem = pDCD->elements;
	_sDCD.numModels = pDCD->models;

	pu8 = &(pDCD->element_data.data[2]);

	_sDCD.numSIGModels = *pu8;

	printf("Num sig models: %d\r\n", _sDCD.numSIGModels);

	pu16 = (uint16_t *) &(pDCD->element_data.data[4]);

	// grab the SIG models from the DCD data
	for (i = 0; i < _sDCD.numSIGModels; i++) {
		_sDCD.SIG_models[i] = *pu16;
		pu16++;
		printf("model ID: %4.4x\r\n", _sDCD.SIG_models[i]);
	}

	pu8 = &(pDCD->element_data.data[3]);

	_sDCD.numVendorModels = *pu8;

	printf("Num vendor models: %d\r\n", _sDCD.numVendorModels);

	pu16 = (uint16_t *) &(pDCD->element_data.data[4 + 2 * _sDCD.numSIGModels + 2]);

	// grab the SIG models from the DCD data
	for (i = 0; i < _sDCD.numVendorModels; i++) {
		_sDCD.vendor_models[i] = *pu16;
		pu16++;
		printf("model ID: %4.4x\r\n", _sDCD.vendor_models[i]);
	}

}

typedef struct {
	// model bindings to be done. for simplicity, all models are bound to same appkey in this example
	// (assuming there is exactly one appkey used and the same appkey is used for all model bindings)
	uint16 bind_model[8];
	uint8 num_bind;
	uint8 num_bind_done;

	// publish addresses for up to 4 models
	uint16 pub_model[8];
	uint16 pub_address[8];
	uint8 num_pub;
	uint8 num_pub_done;

	// subscription addresses for up to 4 models
	uint16 sub_model[8];
	uint16 sub_address[8];
	uint8 num_sub;
	uint8 num_sub_done;

} tsConfig;

// config data to be sent to last provisioned node:
tsConfig _sConfig;

#define LIGHT_CTRL_GRP_ADDR     0xC001
#define LIGHT_STATUS_GRP_ADDR   0xC002

#define MY_VENDOR_ID							0x1111
#define MY_MODEL_SERVER_ID						0x1111
#define MY_MODEL_CLIENT_ID						0x2222
#define MY_MODEL_GRP_ADDR					0xC003

/* models used by simple light example (on/off only)
 * The beta SDK 1.0.1 and 1.1.0 examples are based on these
 * */
#define LIGHT_MODEL_ID            0x1000 // Generic On/Off Server
#define SWITCH_MODEL_ID           0x1001 // Generic On/Off Client

/*
 * Lightness models used in the dimming light example of 1.2.0 SDK
 * */
#define DIM_LIGHT_MODEL_ID              0x1300 // Light Lightness Server
#define DIM_SWITCH_MODEL_ID             0x1302 // Light Lightness Client

/*
 * This function scans for the SIG models in the DCD that was read from a freshly provisioned node.
 * Based on the models that are listed, the publish/subscribe addresses are added into a configuration list
 * that is later used to configure the node.
 *
 * This example configures generic on/off client and lightness client to publish
 * to "light control" group address and subscribe to "light status" group address.
 *
 * Similarly, generic on/off server and lightness server (= the light node) models
 * are configured to subscribe to "light control" and publish to "light status" group address.
 *
 * Alternative strategy for automatically filling the configuration data would be to e.g. use the product ID from the DCD.
 *
 *
 * */
static void config_check() {
	int i;

	memset(&_sConfig, 0, sizeof(_sConfig));

	// scan the SIG models in the DCD data
	for (i = 0; i < _sDCD.numSIGModels; i++) {
		if (_sDCD.SIG_models[i] == SWITCH_MODEL_ID) {
			_sConfig.pub_address[_sConfig.num_pub] = LIGHT_CTRL_GRP_ADDR;
			_sConfig.pub_model[_sConfig.num_pub] = SWITCH_MODEL_ID;
			_sConfig.num_pub++;

			_sConfig.sub_address[_sConfig.num_sub] = LIGHT_STATUS_GRP_ADDR;
			_sConfig.sub_model[_sConfig.num_sub] = SWITCH_MODEL_ID;
			_sConfig.num_sub++;

			_sConfig.bind_model[_sConfig.num_bind] = SWITCH_MODEL_ID;
			_sConfig.num_bind++;
		} else if (_sDCD.SIG_models[i] == LIGHT_MODEL_ID) {
			_sConfig.pub_address[_sConfig.num_pub] = LIGHT_STATUS_GRP_ADDR;
			_sConfig.pub_model[_sConfig.num_pub] = LIGHT_MODEL_ID;
			_sConfig.num_pub++;

			_sConfig.sub_address[_sConfig.num_sub] = LIGHT_CTRL_GRP_ADDR;
			_sConfig.sub_model[_sConfig.num_sub] = LIGHT_MODEL_ID;
			_sConfig.num_sub++;

			_sConfig.bind_model[_sConfig.num_bind] = LIGHT_MODEL_ID;
			_sConfig.num_bind++;

		} else if (_sDCD.SIG_models[i] == DIM_SWITCH_MODEL_ID) {
			_sConfig.pub_address[_sConfig.num_pub] = LIGHT_CTRL_GRP_ADDR;
			_sConfig.pub_model[_sConfig.num_pub] = DIM_SWITCH_MODEL_ID;
			_sConfig.num_pub++;

			_sConfig.sub_address[_sConfig.num_sub] = LIGHT_STATUS_GRP_ADDR;
			_sConfig.sub_model[_sConfig.num_sub] = DIM_SWITCH_MODEL_ID;
			_sConfig.num_sub++;

			_sConfig.bind_model[_sConfig.num_bind] = DIM_SWITCH_MODEL_ID;
			_sConfig.num_bind++;

		} else if (_sDCD.SIG_models[i] == DIM_LIGHT_MODEL_ID) {
			_sConfig.pub_address[_sConfig.num_pub] = LIGHT_STATUS_GRP_ADDR;
			_sConfig.pub_model[_sConfig.num_pub] = DIM_LIGHT_MODEL_ID;
			_sConfig.num_pub++;

			_sConfig.sub_address[_sConfig.num_sub] = LIGHT_CTRL_GRP_ADDR;
			_sConfig.sub_model[_sConfig.num_sub] = DIM_LIGHT_MODEL_ID;
			_sConfig.num_sub++;

			_sConfig.bind_model[_sConfig.num_bind] = DIM_LIGHT_MODEL_ID;
			_sConfig.num_bind++;

		}

	}

	for (i = 0; i < _sDCD.numVendorModels; i++) {
		if (_sDCD.vendor_models[i] == MY_MODEL_SERVER_ID || _sDCD.vendor_models[i] == MY_MODEL_CLIENT_ID) {
			_sConfig.pub_address[_sConfig.num_pub] = MY_MODEL_GRP_ADDR;
			_sConfig.pub_model[_sConfig.num_pub] = _sDCD.vendor_models[i];
			_sConfig.num_pub++;

			_sConfig.sub_address[_sConfig.num_sub] = MY_MODEL_GRP_ADDR;
			_sConfig.sub_model[_sConfig.num_sub] = _sDCD.vendor_models[i];
			_sConfig.num_sub++;

			_sConfig.bind_model[_sConfig.num_bind] = _sDCD.vendor_models[i];
			_sConfig.num_bind++;
		}
	}

}

static void config_retry() {

	uint8 timer_handle = 0;

	switch (state) {
		case waiting_appkey_ack:
			timer_handle = TIMER_ID_APPKEY_ADD;
		break;

		case waiting_bind_ack:
			timer_handle = TIMER_ID_APPKEY_BIND;
		break;

		case waiting_pub_ack:
			timer_handle = TIMER_ID_PUB_SET;
		break;

		case waiting_sub_ack:
			timer_handle = TIMER_ID_SUB_ADD;
		break;

		default:
			printf("config_retry(): don't know how to handle state %d\r\n", state);
		break;
	}

	if (timer_handle > 0) {
		printf("config retry: try step %d again\r\n", state);
		gecko_cmd_hardware_set_soft_timer(TIMER_MS_2_TIMERTICK(500), timer_handle, 1);
	}

}

/**
 * Handling of stack events. Both Bluetooth LE and Bluetooth mesh events are handled here.
 */
static void handle_gecko_event(uint32_t evt_id, struct gecko_cmd_packet *evt) {
	if (NULL == evt) {
		return;
	}

	switch (evt_id) {
		case gecko_evt_system_boot_id:
			// check pushbutton state at startup. If either PB0 or PB1 is held down then do factory reset
			if (GPIO_PinInGet(BSP_BUTTON1_PORT, BSP_BUTTON1_PIN) == 0) {
				initiate_factory_reset();
			} else {
				printf("Initializing as provisioner\r\n");

				state = init;
				// init as provisioner
				struct gecko_msg_mesh_prov_init_rsp_t *prov_init_rsp = gecko_cmd_mesh_prov_init();
				if (prov_init_rsp->result == 0) {
					printf("Successfully initialized\r\n");
				} else {
					printf("Error initializing node as provisioner. Error %x\r\n", prov_init_rsp->result);
				}
			}
		break;

		case gecko_evt_hardware_soft_timer_id:
			switch (evt->data.evt_hardware_soft_timer.handle) {

				case TMIER_ID_BUTTON_POLL:
					button_poll();
				break;

				case TIMER_ID_GET_DCD: {
					struct gecko_msg_mesh_prov_get_dcd_rsp_t* get_dcd_result = gecko_cmd_mesh_prov_get_dcd(provisionee_address, 0xFF);
					if (get_dcd_result->result == 0x0181) {
						printf(".");
						fflush(stdout);
						gecko_cmd_hardware_set_soft_timer(TIMER_MS_2_TIMERTICK(1000), TIMER_ID_GET_DCD, 1);
					} else if (get_dcd_result->result != 0x0) {
						printf("gecko_cmd_mesh_prov_get_dcd failed with result 0x%X (%s) addr %x\r\n", get_dcd_result->result, res2str(get_dcd_result->result), provisionee_address);
						gecko_cmd_hardware_set_soft_timer(TIMER_MS_2_TIMERTICK(1000), TIMER_ID_GET_DCD, 1);
					} else {
						printf("requesting DCD from the node...\r\n");
						state = waiting_dcd;
					}

				}
				break;

				case TIMER_ID_APPKEY_ADD: {
					struct gecko_msg_mesh_prov_appkey_add_rsp_t *appkey_deploy_evt;
					appkey_deploy_evt = gecko_cmd_mesh_prov_appkey_add(provisionee_address, netkey_id, appkey_id);
					if (appkey_deploy_evt->result == 0) {
						printf("Appkey deployed to %x\r\n", provisionee_address);
						state = waiting_appkey_ack;
					} else {
						printf("Appkey deployment failed. addr %x, error: %x\r\n", provisionee_address, appkey_deploy_evt->result);
						gecko_cmd_hardware_set_soft_timer(TIMER_MS_2_TIMERTICK(500), TIMER_ID_APPKEY_ADD, 1);

					}
				}
				break;

				case TIMER_ID_APPKEY_BIND: {
					uint16 vendor_id = 0xFFFF; // configuring only SIG models for now
					uint16 model_id;

					// take the next model from the list of models to be bound with application key.
					// for simplicity, the same appkey is used for all models but it is possible to also use several appkeys
					model_id = _sConfig.pub_model[_sConfig.num_bind_done];

					printf("\r\nAPP_BIND, config %d/%d:: model %4.4x key index %x\r\n", _sConfig.num_bind_done + 1, _sConfig.num_bind, model_id, appkey_id);
					if (_sConfig.num_bind_done + 1 == _sConfig.num_bind) {
						/*last one*/
						vendor_id = 0x1111;
					}
					printf("Vendor_id = 0x%04X, Model_id = 0x%04X\r\n", vendor_id, model_id);
					struct gecko_msg_mesh_prov_model_app_bind_rsp_t *model_app_bind_result = gecko_cmd_mesh_prov_model_app_bind(provisionee_address, provisionee_address, netkey_id, appkey_id, vendor_id, model_id);

					if (model_app_bind_result->result == STATUS_OK) {
						printf("success - waiting bind ack\r\n");
						state = waiting_bind_ack;
					} else if (model_app_bind_result->result == STATUS_BUSY) {
						printf(".");
						fflush(stdout);
						gecko_cmd_hardware_set_soft_timer(TIMER_MS_2_TIMERTICK(500), TIMER_ID_APPKEY_BIND, 1);
					} else if (model_app_bind_result->result != STATUS_OK) {
						printf("prov_model_app_bind failed with result 0x%X\r\n", model_app_bind_result->result);
						gecko_cmd_hardware_set_soft_timer(TIMER_MS_2_TIMERTICK(500), TIMER_ID_APPKEY_BIND, 1);
					}
				}
				break;

				case TIMER_ID_PUB_SET: {
					uint16 vendor_id = 0xFFFF; // configuring only SIG models for now
					uint16 model_id;
					uint16 pub_address;

					// get the next model/address pair from the configuration list:
					model_id = _sConfig.pub_model[_sConfig.num_pub_done];
					pub_address = _sConfig.pub_address[_sConfig.num_pub_done];

					printf("\r\npublish set, config %d/%d: model %4.4x -> address %4.4x\r\n", _sConfig.num_pub_done + 1, _sConfig.num_pub, model_id, pub_address);
					if (_sConfig.num_pub_done + 1 == _sConfig.num_pub) {
						/*last one*/
						vendor_id = 0x1111;
					}
					printf("Vendor_id = 0x%04X, Model_id = 0x%04X\r\n", vendor_id, model_id);
					struct gecko_msg_mesh_prov_model_pub_set_rsp_t *model_pub_set_result = gecko_cmd_mesh_prov_model_pub_set(provisionee_address, provisionee_address, netkey_id, appkey_id, vendor_id, model_id, pub_address, 3, /* Publication time-to-live value */
					0, /* period = NONE */
					0 /* model publication retransmissions */
					);

					if (model_pub_set_result->result == STATUS_OK) {
						printf("success - waiting pub ack\r\n");
						state = waiting_pub_ack;
					} else if (model_pub_set_result->result == STATUS_BUSY) {
						printf(".");
						fflush(stdout);
					} else if (model_pub_set_result->result != STATUS_OK) {
						printf("prov_model_pub_set failed with result 0x%X\r\n", model_pub_set_result->result);
					}
				}
				break;

				case TIMER_ID_SUB_ADD: {
					uint16 vendor_id = 0xFFFF; // configuring only SIG models for now
					uint16 model_id;
					uint16 sub_address;

					// get the next model/address pair from the configuration list:
					model_id = _sConfig.sub_model[_sConfig.num_sub_done];
					sub_address = _sConfig.sub_address[_sConfig.num_sub_done];

					printf("\r\nsubscription add, config %d/%d: model %4.4x -> address %4.4x\r\n", _sConfig.num_sub_done + 1, _sConfig.num_sub, model_id, sub_address);
					if (_sConfig.num_sub_done + 1 == _sConfig.num_sub) {
						/*last one*/
						vendor_id = 0x1111;
					}
					printf("Vendor_id = 0x%04X, Model_id = 0x%04X\r\n", vendor_id, model_id);
					struct gecko_msg_mesh_prov_model_sub_add_rsp_t *model_sub_add_result = gecko_cmd_mesh_prov_model_sub_add(provisionee_address, provisionee_address, netkey_id, vendor_id, model_id, sub_address);

					if (model_sub_add_result->result == STATUS_OK) {
						printf("success - waiting sub ack\r\n");
						state = waiting_sub_ack;
					}
					if (model_sub_add_result->result == STATUS_BUSY) {
						printf(".");
						fflush(stdout);
					} else if (model_sub_add_result->result != STATUS_OK) {
						printf("prov_model_sub_add failed with result 0x%X\r\n", model_sub_add_result->result);
					}

				}
				break;

				case TIMER_ID_FACTORY_RESET:
					gecko_cmd_system_reset(0);
				break;

				case TIMER_ID_RESTART:
					gecko_cmd_system_reset(0);
				break;

				default:
				break;
			}

		break;

		case gecko_evt_mesh_prov_dcd_status_id: {
			struct gecko_msg_mesh_prov_dcd_status_evt_t *pDCD = (struct gecko_msg_mesh_prov_dcd_status_evt_t *) &(evt->data);
			printf("DCD status event. result = %x\r\n", pDCD->result);

			if (pDCD->result == 0) {
				// decode the DCD content
				DCD_decode(pDCD);

				// check the desired configuration settings depending on what's in the DCD
				config_check();

				// next step : send appkey to device
				gecko_cmd_hardware_set_soft_timer(TIMER_MS_2_TIMERTICK(500), TIMER_ID_APPKEY_ADD, 1);
			} else {
				printf("DCD status: %x\r\n", pDCD->result);
			}

		}
		break;

		case gecko_evt_mesh_prov_config_status_id: {
			struct gecko_msg_mesh_prov_config_status_evt_t *conf_status_evt = (struct gecko_msg_mesh_prov_config_status_evt_t *) &evt->data;

			printf("mesh_prov_config_status: addr = 0x%X, id = 0x%X, status = 0x%X\r\n", conf_status_evt->address, conf_status_evt->id, conf_status_evt->status);

			if (conf_status_evt->status) {
				printf("Not successful, will try again\n");
				config_retry();
			} else {
				// move to next phase in configuration

				if (state == waiting_appkey_ack) {
					gecko_cmd_hardware_set_soft_timer(TIMER_MS_2_TIMERTICK(500), TIMER_ID_APPKEY_BIND, 1);
				} else if (state == waiting_bind_ack) {
					printf("bind complete\r\n");
					_sConfig.num_bind_done++;

					if (_sConfig.num_bind_done < _sConfig.num_bind) {
						// more model<->appkey bindings to be done
						gecko_cmd_hardware_set_soft_timer(TIMER_MS_2_TIMERTICK(500), TIMER_ID_APPKEY_BIND, 1);
					} else {
						gecko_cmd_hardware_set_soft_timer(TIMER_MS_2_TIMERTICK(500), TIMER_ID_PUB_SET, 1);
					}
				} else if (state == waiting_pub_ack) {
					printf("PUB complete\r\n");
					_sConfig.num_pub_done++;

					if (_sConfig.num_pub_done < _sConfig.num_pub) {
						// more publication settings to be done
						gecko_cmd_hardware_set_soft_timer(TIMER_MS_2_TIMERTICK(500), TIMER_ID_PUB_SET, 1);
					} else {
						gecko_cmd_hardware_set_soft_timer(TIMER_MS_2_TIMERTICK(500), TIMER_ID_SUB_ADD, 1);
					}
				} else if (state == waiting_sub_ack) {
					printf("SUB complete\r\n");
					_sConfig.num_sub_done++;
					if (_sConfig.num_sub_done < _sConfig.num_sub) {
						// more subscription settings to be done
						gecko_cmd_hardware_set_soft_timer(TIMER_MS_2_TIMERTICK(500), TIMER_ID_SUB_ADD, 1);
					} else {
						printf("configuration complete\r\n");
						state = scanning;
					}

				} else {
					printf("unexpected prov conf status: state = %d\r\n", state);
				}

			}

		}
		break;

		case gecko_evt_le_gap_adv_timeout_id:
			// adv timeout events silently discarded
		break;

		case gecko_evt_le_connection_opened_id:
			printf("evt:gecko_evt_le_connection_opened_id\r\n");
			num_connections++;
			conn_handle = evt->data.evt_le_connection_opened.connection;
		break;

		case gecko_evt_le_connection_parameters_id:
			printf("evt:gecko_evt_le_connection_parameters_id\r\n");
		break;

		case gecko_evt_le_connection_closed_id:
			printf("evt:conn closed, reason 0x%x\r\n", evt->data.evt_le_connection_closed.reason);
			conn_handle = 0xFF;
		break;

		case gecko_evt_gatt_server_user_write_request_id:

		break;

		case gecko_evt_system_external_signal_id: {

		}
		break;

		case gecko_evt_mesh_prov_initialized_id: {
			struct gecko_msg_mesh_prov_initialized_evt_t *initialized_evt;
			initialized_evt = (struct gecko_msg_mesh_prov_initialized_evt_t *) &(evt->data);

			printf("gecko_cmd_mesh_prov_init_id\r\n");
			printf("networks: %x\r\n", initialized_evt->networks);
			printf("address: %x\r\n", initialized_evt->address);
			printf("ivi: %x\r\n", (unsigned int) initialized_evt->ivi);

			if (initialized_evt->networks > 0) {
				printf("network keys already exist\r\n");
				netkey_id = 0;
				appkey_id = 0;
			} else {
				printf("Creating a new netkey\r\n");

				struct gecko_msg_mesh_prov_create_network_rsp_t *new_netkey_rsp;
				new_netkey_rsp = gecko_cmd_mesh_prov_create_network(0, (const uint8 *) "");

				if (new_netkey_rsp->result == 0) {
					netkey_id = new_netkey_rsp->network_id;
					printf("Success, netkey id = %x\r\n", netkey_id);
				} else {
					printf("Failed to create new netkey. Error: %x", new_netkey_rsp->result);
				}

				printf("Creating a new appkey\r\n");

				struct gecko_msg_mesh_prov_create_appkey_rsp_t *new_appkey_rsp;
				new_appkey_rsp = gecko_cmd_mesh_prov_create_appkey(netkey_id, 0, (const uint8 *) "");

				if (new_netkey_rsp->result == 0) {
					appkey_id = new_appkey_rsp->appkey_index;
					printf("Success, appkey_id = %x\r\n", appkey_id);
					printf("Appkey: ");
					for (uint32_t i = 0; i < new_appkey_rsp->key.len; ++i) {
						printf("%02x ", new_appkey_rsp->key.data[i]);
					}
					printf("\r\n");
				} else {
					printf("Failed to create new appkey. Error: %x", new_appkey_rsp->result);
				}
			}

			printf("Starting to scan for unprovisioned device beacons\r\n");

			struct gecko_msg_mesh_prov_scan_unprov_beacons_rsp_t *scan_rsp;
			scan_rsp = gecko_cmd_mesh_prov_scan_unprov_beacons();

			if (scan_rsp->result == 0) {
				printf("Success - initializing unprovisioned beacon scan\r\n");
				state = scanning;
			} else {
				printf("Failure initializing unprovisioned beacon scan. Result: %x\r\n", scan_rsp->result);
			}

			// start timer for button polling
			gecko_cmd_hardware_set_soft_timer(TIMER_MS_2_TIMERTICK(100), TMIER_ID_BUTTON_POLL, 0);

			break;
		}

		case gecko_evt_mesh_prov_unprov_beacon_id: {
			struct gecko_msg_mesh_prov_unprov_beacon_evt_t *beacon_evt = (struct gecko_msg_mesh_prov_unprov_beacon_evt_t *) &(evt->data);
			int i;
			if ((state == scanning) && (ask_user_input == false)) {
				printf("gecko_evt_mesh_prov_unprov_beacon_id\r\n");

				for (i = 0; i < beacon_evt->uuid.len; i++) {
					printf("%2.2x", beacon_evt->uuid.data[i]);
				}
				printf("\r\n");

				memcpy(uuid_copy_buf, beacon_evt->uuid.data, 16);
				printf("confirm?\r\n");
				// suspend reporting of unprov beacons until user has rejected or accepted this one using buttons PB0 / PB1
				ask_user_input = true;
			}
			break;
		}

		case gecko_evt_mesh_prov_provisioning_failed_id: {
			struct gecko_msg_mesh_prov_provisioning_failed_evt_t *fail_evt = (struct gecko_msg_mesh_prov_provisioning_failed_evt_t*) &(evt->data);

			printf("Provisioning failed. Reason: %x\r\n", fail_evt->reason);
			state = scanning;

			break;
		}

		case gecko_evt_mesh_prov_device_provisioned_id: {
			struct gecko_msg_mesh_prov_device_provisioned_evt_t *prov_evt = (struct gecko_msg_mesh_prov_device_provisioned_evt_t*) &(evt->data);

			printf("Node successfully provisioned. Address: %4.4x\r\n", prov_evt->address);
			state = provisioned;

			printf("provisioning done - uuid 0x");
			for (uint8_t i = 0; i < prov_evt->uuid.len; i++)
				printf("%02X", prov_evt->uuid.data[i]);
			printf("\r\n");

			provisionee_address = prov_evt->address;

			/* kick of next phase which is reading DCD from the newly provisioned node */
			gecko_cmd_hardware_set_soft_timer(TIMER_MS_2_TIMERTICK(500), TIMER_ID_GET_DCD, 1);

			break;

			break;
		}

		default:
			printf("unhandled evt: %8.8x class %2.2x method %2.2x\r\n", (unsigned int) evt_id, (unsigned int) ((evt_id >> 16) & 0xFF), (unsigned int) ((evt_id >> 24) & 0xFF));
		break;
	}
}
