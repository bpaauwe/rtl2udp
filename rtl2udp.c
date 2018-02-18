#define _GNU_SOURCE

#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <math.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include "cJSON.h"

struct air_data {
	double temperature;
	double humidity;
	double pressure;
	double battery;
	int sensor;
	int time;
	int interval;
};

struct sky_data {
	double wind_speed;
	double gust_speed;
	double wind_direction;
	double rainfall;
	double battery;
	int sensor;
	int time;
	int interval;
};

static int fp_getline(FILE *fp, char *s, int lim);
static void parse_air(cJSON *msg_json, struct air_data *data);
static void parse_sky(cJSON *msg_json, struct sky_data *data);
static void publish_air(struct air_data *data);
static void publish_sky(struct sky_data *sky_data);
static void send_json(char *packet);

static double tempc(double tempf) {
	return round(((tempf  - 32) / 1.8) * 10) / 10;
}

static double mph2ms(double mph) {
	return round((mph * .44704) * 10) / 10;
}

static double in2mm(double in) {
	return round((in * 25.4) * 10) / 10;
}

int main (int argc, char **argv)
{
	char line[512];
	cJSON *msg_json;
	const cJSON *field;
	struct air_data air;
	struct sky_data sky;

	air.time = time(NULL);
	sky.time = time(NULL);

	while (fp_getline(stdin, line, 512)) {
		msg_json = cJSON_Parse(line);

		if (msg_json == NULL) {
			const char *error_ptr = cJSON_GetErrorPtr();
			if (error_ptr != NULL) {
				fprintf(stderr, "Error before: %s\n", error_ptr);
			}
			goto skip_message;
		}


		field = cJSON_GetObjectItemCaseSensitive(msg_json, "model");
		/*
		if (cJSON_IsString(field) && (field->valuestring != NULL)) {
			if (strcmp(field->valuestring, "Acurite 5n1 sensor"))
				goto skip_message;

			printf("Model: %s\n", field->valuestring);
		}
		*/
		field = cJSON_GetObjectItemCaseSensitive(msg_json, "sequence_num");
		if (field && (field->valueint != 0))
			continue;

		field = cJSON_GetObjectItemCaseSensitive(msg_json, "message_type");
		if (field) {
			printf("type: %d\n", field->valueint);

		/* Parse info based on message type? */
		/*
		 * type 56:
		 *   "wind_speed_mph" : 3.193,
		 *   "temperature_F" : 54.500,
		 *   "humidity" : 53
		 *   "sequence_num" : 0  [maybe skip any other sequence_num value]
		 *
		 * type 49:
		 *   "wind_speed_mph" : 3.193
		 *   "wind_dir_deg" : 292.500,
		 *   "wind_dir" : "WNW"
		 *   "rainfall_accumulation_inch" : 0.000,
		 *   raincounter_raw" : 0
		 */
			switch (field->valueint) {
				case 56:
					parse_air(msg_json, &air);
					publish_air(&air);
					break;
				case 49:
					parse_sky(msg_json, &sky);
					publish_sky(&sky);
					break;
				default:
					printf("Message type %d\n", field->valueint);
					printf("%s\n\n", line);
					break;
			}
		}


skip_message:
		cJSON_Delete(msg_json);
		//printf("%s", line);
		//
	}

}


/*
 * Parse the sensor data and build a UDP packet that simulates a
 * WF Air packet.
 */

static void parse_air(cJSON *msg_json, struct air_data *air_data)
{
	cJSON *field;

	field = cJSON_GetObjectItemCaseSensitive(msg_json, "sensor_id");
	if (field)
		air_data->sensor = field->valueint;

	field = cJSON_GetObjectItemCaseSensitive(msg_json, "battery");
	if (field) {
		if (strcmp(field->valuestring, "OK") == 0)
			air_data->battery = 3.0;
		else
			air_data->battery = 2.0;
	}

	field = cJSON_GetObjectItemCaseSensitive(msg_json, "temperature_F");
	if (field)
		air_data->temperature = tempc(field->valuedouble);

	field = cJSON_GetObjectItemCaseSensitive(msg_json, "humidity");
	if (field)
		air_data->humidity = field->valuedouble;

	air_data->interval = time(NULL) - air_data->time;
	air_data->time = time(NULL);

	/* TODO: Read pressure sensor */
	air_data->pressure = 998.3;

}

static void parse_sky(cJSON *msg_json, struct sky_data *sky_data)
{
	cJSON *field;

	field = cJSON_GetObjectItemCaseSensitive(msg_json, "sensor_id");
	if (field)
		sky_data->sensor = field->valueint;

	field = cJSON_GetObjectItemCaseSensitive(msg_json, "battery");
	if (field) {
		if (strcmp(field->valuestring, "OK") == 0)
			sky_data->battery = 3.0;
		else
			sky_data->battery = 2.0;
	}

	field = cJSON_GetObjectItemCaseSensitive(msg_json, "wind_speed_mph");
	if (field) {
		sky_data->wind_speed = mph2ms(field->valuedouble);

		if (sky_data->wind_speed > sky_data->gust_speed)
			sky_data->gust_speed = sky_data->wind_speed;
	}

	field = cJSON_GetObjectItemCaseSensitive(msg_json, "wind_dir_deg");
	if (field)
		sky_data->wind_direction = field->valuedouble;

	field = cJSON_GetObjectItemCaseSensitive(msg_json,
			"rainfall_accumulation_inch");
	if (field)
		sky_data->rainfall = in2mm(field->valuedouble);

	sky_data->interval = time(NULL) - sky_data->time;
	sky_data->time = time(NULL);
}


static void publish_air(struct air_data *air_data)
{
	cJSON *air = NULL;
	cJSON *obs = NULL;
	cJSON *ob = NULL;
	char serial_number[15];

	sprintf(serial_number, "ACU-%d", air_data->sensor);
	air = cJSON_CreateObject();

	cJSON_AddStringToObject(air, "serial_number", serial_number);
	cJSON_AddStringToObject(air, "type", "obs_air");
	cJSON_AddStringToObject(air, "hub_sn", "5n1");
	obs = cJSON_AddArrayToObject(air, "obs");
	ob = cJSON_AddArrayToObject(obs, "");
	cJSON_AddNumberToObject(ob, "", air_data->time); /* Time Epoch */
	cJSON_AddNumberToObject(ob, "", air_data->pressure);
	cJSON_AddNumberToObject(ob, "", air_data->temperature);
	cJSON_AddNumberToObject(ob, "", air_data->humidity);
	cJSON_AddNumberToObject(ob, "", 0);  /* Lightning Strike Count */
	cJSON_AddNumberToObject(ob, "", 0);  /* Lightning Strike Avg Distance */
	cJSON_AddNumberToObject(ob, "", air_data->battery);
	cJSON_AddNumberToObject(ob, "", air_data->interval);
	cJSON_AddNumberToObject(air, "firmware_revision", 17);

	send_json(cJSON_Print(air));

	cJSON_Delete(air);
}

static void publish_sky(struct sky_data *sky_data)
{
	cJSON *sky = NULL;
	cJSON *obs = NULL;
	cJSON *ob = NULL;
	char serial_number[15];

	sprintf(serial_number, "ACU-%d", sky_data->sensor);
	sky = cJSON_CreateObject();

	cJSON_AddStringToObject(sky, "serial_number", serial_number);
	cJSON_AddStringToObject(sky, "type", "obs_sky");
	cJSON_AddStringToObject(sky, "hub_sn", "5n1");
	obs = cJSON_AddArrayToObject(sky, "obs");
	ob = cJSON_AddArrayToObject(obs, "");
	cJSON_AddNumberToObject(ob, "", sky_data->time); /* Time Epoch */
	cJSON_AddNumberToObject(ob, "", 0);  /* Illuminance  */
	cJSON_AddNumberToObject(ob, "", 0);  /* UV */
	cJSON_AddNumberToObject(ob, "", sky_data->rainfall);
	cJSON_AddNumberToObject(ob, "", 0);  /* Wind Lull */
	cJSON_AddNumberToObject(ob, "", sky_data->wind_speed);
	cJSON_AddNumberToObject(ob, "", sky_data->gust_speed);
	cJSON_AddNumberToObject(ob, "", sky_data->wind_direction);
	cJSON_AddNumberToObject(ob, "", sky_data->battery);
	cJSON_AddNumberToObject(ob, "", sky_data->interval);
	cJSON_AddNumberToObject(ob, "", 0);  /* Solar Radiation */
	cJSON_AddNumberToObject(ob, "", 0);  /* Local Day Rain */
	cJSON_AddNumberToObject(sky, "firmware_revision", 17);

	send_json(cJSON_Print(sky));

	cJSON_Delete(sky);
}

static void send_json(char *packet)
{
	int bcast_sock;
	int enable_broadcast = 1;
	struct sockaddr_in s;
	int ret;

	printf("Attempting to broadcast\n");
	printf("%s\n", packet);

	bcast_sock = socket(AF_INET, SOCK_DGRAM, 0);
	ret = setsockopt(bcast_sock, SOL_SOCKET, SO_BROADCAST,
			&enable_broadcast, sizeof(enable_broadcast));

	if (ret) {
		printf("Failed to create socket\n");
		goto out;
	}

	memset(&s, 0, sizeof(struct sockaddr_in));
	s.sin_family = AF_INET;
	s.sin_port = (in_port_t)htons(50222);
	s.sin_addr.s_addr = htonl(INADDR_BROADCAST);

	if (sendto(bcast_sock, packet, strlen(packet), 0, (struct sockaddr *)&s,
				sizeof(struct sockaddr_in)) < 0) {
		perror("sendto");
	}

out:
	close(bcast_sock);
}

static int fp_getline(FILE *fp, char *s, int lim)
{
	int c, i;
	int ws = 0;

	i = 0;

	while (--lim > 0 && (c = fgetc(fp)) != EOF && c != '\n') {
		if (c != '\r') {
			if (!ws && ((c == ' ') || (c == '\t'))) {
				/* skip leading white space */
			} else {
				s[i++] = c;
				ws = 1;
			}
		}
	}

	s[i] = '\0';
	if ( c == EOF ) {
		return (c);
	} else {
		return(i);
	}
}
