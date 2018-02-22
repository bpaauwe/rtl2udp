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
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
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
	double illumination;
	double battery;
	int sensor;
	int time;
	int interval;
	int precip_type;
};

static int fp_getline(FILE *fp, char *s, int lim);
static void parse_air(cJSON *msg_json, struct air_data *data);
static void parse_sky(cJSON *msg_json, struct sky_data *data);
static void publish_air(struct air_data *data);
static void publish_sky(struct sky_data *sky_data);
static void send_json(char *packet);
static void get_pressure(struct air_data *air);
static void get_lux(struct sky_data *sky);

static double tempc(double tempf) {
	return round(((tempf  - 32) / 1.8) * 10) / 10;
}

static double mph2ms(double mph) {
	return round((mph * .44704) * 10) / 10;
}

static double in2mm(double in) {
	return round((in * 25.4) * 10) / 10;
}

static int debug = 0;

int main (int argc, char **argv)
{
	char line[512];
	cJSON *msg_json;
	const cJSON *field;
	struct air_data air;
	struct sky_data sky;
	int i;

	air.time = time(NULL);
	sky.time = time(NULL);

	if (argc > 1) {
		for(i = 1; i < argc; i++) {
			if (argv[i][0] == '-') { /* An option */
				switch (argv[i][1]) {
					case 'd': /* debug */
						if (++i < argc)
							debug = atoi(argv[++i]);
						else
							debug = 1;
						break;
					default:
						printf("usage: %s [-d]\n", argv[0]);
						break;
				}
			}
		}
	}

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
					get_pressure(&air);
					publish_air(&air);
					break;
				case 49:
					parse_sky(msg_json, &sky);
					get_lux(&sky);
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
	}
	return 0;
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
	if (field) {
		sky_data->rainfall = in2mm(field->valuedouble);
		if (sky_data->rainfall > 0)
			sky_data->precip_type = 1;
	}

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
	cJSON_AddNumberToObject(air, "firmware_revision", 35);

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
	cJSON_AddNumberToObject(ob, "", sky_data->illumination); /* Lux  */
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
	cJSON_AddNumberToObject(ob, "", sky_data->precip_type);
	cJSON_AddNumberToObject(ob, "", 0);  /* wind sample interval */
	cJSON_AddNumberToObject(sky, "firmware_revision", 35);

	send_json(cJSON_Print(sky));

	cJSON_Delete(sky);
}

static void send_json(char *packet)
{
	int bcast_sock;
	int enable_broadcast = 1;
	struct sockaddr_in s;
	int ret;

	if (debug > 1) {
		printf("Attempting to broadcast\n");
		printf("%s\n", packet);
	}

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

static void get_lux(struct sky_data *sky)
{
	int i2c;
	unsigned char reg[1];
	unsigned char config[2];
	unsigned char data[24];
	int ch0, ch1;

	/* Open the I2C bus */
	i2c = open("/dev/i2c-1", O_RDWR);
	if (i2c < 0) {
		fprintf(stderr, "Failed to open I2C bus.\n");
		return;
	}

	/* Connect to the BMP280 device (address = 0x39) */
	ioctl(i2c, I2C_SLAVE, 0x39);

	/*
	 * Power on mode
	 */
	config[0] = 0x00 | 0x80;
	config[1] = 0x03;
	if (write(i2c, config, 2) < 0)
		fprintf(stderr, "Failed to write control measurement register.\n");

	/*
	 * timing register
	 *  - Mominal integration time = 402ms
	 */
	config[0] = 0x01 | 0x80;
	config[1] = 0x02;
	if (write(i2c, config, 2) < 0)
		fprintf(stderr, "Failed to write control measurement register.\n");

	sleep(1);

	/* Read Lux data */
	reg[0] = 0x0C | 0x80;
	if (write(i2c, reg, 1) < 0)
		goto end_lux;

	if (read(i2c, data, 4) < 0)
		goto end_lux;

	/*
	 * ch0 is full spectrum (IR + Visible)
	 * ch1 is IR only
	 * ch0 - ch1 is visible only
	 */
	ch0 = data[1] * 256 + data[0];
	ch1 = data[3] * 256 + data[2];

	/* Return the visible only reading */
	sky->illumination = (double)(ch0 - ch1);

	if (debug) {
		printf("Full : %d lux\n", ch0);
		printf("IR   : %d lux\n", ch1);
		printf("VIS  : %d lux\n", (ch0 - ch1));
	}

end_lux:
	close(i2c);
	return;
}

struct bmp_280_calibration {
	double T1;
	double T2;
	double T3;
	double P1;
	double P2;
	double P3;
	double P4;
	double P5;
	double P6;
	double P7;
	double P8;
	double P9;
};

/*
 * These values are in the device as 16 bit signed shorts
 * Use this macro to convert them to doubles.
 */
#define COEF(d, i) { \
	d = (double)(data[i+1] * 256 + data[i]); \
	if (d > 32767) \
		d -= 65536; \
}

static struct bmp_280_calibration *bmp_280 = NULL;
static void get_pressure(struct air_data *air)
{
	int i2c;
	unsigned char reg[1];
	unsigned char config[2];
	unsigned char data[24];
	long pres;
	long temp;
	double var1, var2, p, pressure, t_fine;

	/* Open the I2C bus */
	i2c = open("/dev/i2c-1", O_RDWR);
	if (i2c < 0) {
		fprintf(stderr, "Failed to open I2C bus.\n");
		return;
	}

	/* Connect to the BMP280 device (address = 0x77) */
	ioctl(i2c, I2C_SLAVE, 0x77);

	/* Get coefficient data for the sensors, but do it only once */
	if (!bmp_280) {
		bmp_280 = (struct bmp_280_calibration *)
			malloc(sizeof(struct bmp_280_calibration));

		/* Read calibration data */
		reg[0] = 0x88;
		if (write(i2c, reg, 1) < 0)
			goto end_pres;

		if (read(i2c, data, 24) > 0) {
			/* temperature coefficents */
			bmp_280->T1 = (double)(data[1] * 256 + data[0]);
			COEF(bmp_280->T2, 2);
			COEF(bmp_280->T3, 4);

			/* pressure coefficents */
			bmp_280->P1 = (double)(data[7] * 256 + data[6]);
			COEF(bmp_280->P2, 8);
			COEF(bmp_280->P3, 10);
			COEF(bmp_280->P4, 12);
			COEF(bmp_280->P5, 14);
			COEF(bmp_280->P6, 16);
			COEF(bmp_280->P7, 18);
			COEF(bmp_280->P8, 20);
			COEF(bmp_280->P9, 22);
		} else {
			free (bmp_280);
			fprintf(stderr, "Failed to read coefficent data.\n");
			goto end_pres;
		}
	}

	/*
	 * Control measurement register
	 *  - norma mode
	 *  - over sample rate = 1
	 */
	config[0] = 0xF4;
	config[1] = 0x27;
	if (write(i2c, config, 2) < 0)
		fprintf(stderr, "Failed to write control measurement register.\n");

	/*
	 * Config register
	 *  - standby time = 1000ms
	 */
	config[0] = 0xF5;
	config[1] = 0xA0;
	if (write(i2c, config, 2) < 0)
		fprintf(stderr, "Failed to write control measurement register.\n");

	sleep(1);

	/* Read temp and pressure data */
	reg[0] = 0xF7;
	if (write(i2c, reg, 1) < 0)
		goto end_pres;

	if (read(i2c, data, 8) < 0)
		goto end_pres;

	/* Temperature calculation */
	temp = (((long)data[3] << 16) | ((long)data[4] << 8) |
			((long)data[5] & 0xF0)) / 16;
	/*
	temp = (((long)data[3] * 65536) | ((long)data[4] * 256) |
			((long)data[5] & 0xF0)) / 16;
			*/

	var1 = (((double)temp / 16384) - (bmp_280->T1 / 1024)) * bmp_280->T2;
	var2 = (((double)temp / 131072) - (bmp_280->T1 / 8192)) *
		(((double)temp / 131072) - (bmp_280->T1 / 8192)) * bmp_280->T3;
	t_fine = var1 + var2;

	if (debug)
		printf("Indoor temp = %.1f F\n", (((t_fine / 5120) * 1.8) + 32));

	/* Pressure calculation */
	pres = (((long)data[0] << 16) | ((long)data[1] << 8) |
			((long)data[2] & 0xF0)) / 16;

	var1 = (t_fine / 2) - 64000;
	var2 = var1 * var1 * (bmp_280->P6 / 32768);
	var2 = var2 + var1 * (bmp_280->P5 * 2);
	var2 = (var2 / 4) + (bmp_280->P4 * 65536);
	var1 = (bmp_280->P3 * var1 * var1 / 524288 + bmp_280->P2 * var1) / 524288;
	var1 = (1 + var1 / 32768) * bmp_280->P1;

	p = 1048576 - pres;
	p = (p - (var2 / 4096)) * 6250 / var1;

	var1 = bmp_280->P9 * p * p / 2147483648;
	var2 = p * bmp_280->P8 / 32768;

	pressure = (p + (var1 + var2 + bmp_280->P7) / 16) / 100;
	if (debug)
		printf("Pressure = %.1f hPa\n", pressure);

	/*
	 * This is station pressure.  If we want sea level, it will need
	 * to be converted.
	 */
	air->pressure = pressure;

end_pres:
	close(i2c);
	return;
}
