#define _GNU_SOURCE
#include <getopt.h>
#include <string.h>
#include <unistd.h>

#include <zip.h>
#include <json-c/json.h>

#include "conf.h"
#include "log.h"
#include "serialtty.h"
#include "dfu.h"

struct config conf;
int ser_fd = -1;

static struct option options[] = {
	{ "help",	no_argument,		NULL, 'h' },
	{ "debug",      optional_argument,	NULL, 'd' },
	{ "port",       required_argument,	NULL, 'p' },
	{ "cmd",        required_argument,	NULL, 'c' },
	{ NULL, 0, NULL, 0 }
};

static void usage(void)
{
	fprintf(stderr, "Usage: nrfserdfu [options] DFUPKG.zip\n"
			"Nordic NRF DFU Upgrade with DFUPKG.zip\n"
			"Options:\n"
			"  -h, --help\t\tShow help\n"
			"  -d, --debug=<level>\tDebug level (1 or 2)\n"
			"  -p, --port <tty>\tSerial port (/dev/ttyUSB0)\n"
			"  -c, --cmd <text>\tCommand to enter DFU mode\n");
}

static void main_options(int argc, char* argv[])
{
	/* defaults */
	conf.serport = "/dev/ttyUSB0";
	conf.debug = LL_NOTICE;

	int n = 0;
	while (n >= 0) {
		n = getopt_long(argc, argv, "hd::p:c:", options, NULL);
		if (n < 0)
			continue;
		switch (n) {
		case 'h':
			usage();
			break;
		case 'd':
			if (optarg == NULL)
				conf.debug = LL_INFO;
			else if (atoi(optarg) == 2)
				conf.debug = LL_DEBUG;
			break;
		case 'p':
			conf.serport = optarg;
			break;
		case 'c':
			conf.dfucmd = optarg;
			break;
		}
	}

	/* first non-option argument is ZIP file */
	if (optind < argc) {
		conf.zipfile = argv[optind++];
	}

	if (optind < argc) {
		LOG_ERR("Garbage arguments from %s",argv[optind]);
	}
}

static zip_file_t* zip_file_open(zip_t* zip, const char* name, size_t* size)
{
	struct zip_stat stat;

	zip_stat_init(&stat);
	int ret = zip_stat(zip, name, 0, &stat);
	if (ret < 0) {
		LOG_ERR("ZIP file does not contain %s", name);
		return NULL;
	}
	*size = stat.size;

	zip_file_t* zf = zip_fopen_index(zip, stat.index, 0);
	if (zf == NULL) {
		LOG_ERR("Error opening %s in ZIP file", name);
		return NULL;
	}
	return zf;
}

/* dat and bin have to be freed by caller */
static int read_manifest(zip_t* zip, char** dat, char** bin)
{
	char buf[200];

	zip_file_t* zf = zip_fopen(zip, "manifest.json", 0);
	if (zf == NULL) {
		LOG_ERR("ZIP file does not contain manifest");
		return -1;
	}

	zip_int64_t len = zip_fread(zf, buf, sizeof(buf));
	if (len <= 0) {
		LOG_ERR("Could not read Manifest");
		return -1;
	}

	/* read JSON */
	json_object* json;
	json_object* jobj;
	json_object* jobj2;
	json = json_tokener_parse(buf);
	if (json == NULL) {
		LOG_ERR("Manifest not valid JSON");
		zip_fclose(zf);
		return -1;
	}

	if (json_object_object_get_ex(json, "manifest", &jobj) &&
	    json_object_object_get_ex(jobj, "application", &jobj2)) {
		if (json_object_object_get_ex(jobj2, "dat_file", &jobj)) {
			*dat = strdup(json_object_get_string(jobj));
		}
		if (json_object_object_get_ex(jobj2, "bin_file", &jobj)) {
			*bin = strdup(json_object_get_string(jobj));
		}
	}
	json_object_put(json);
	zip_fclose(zf);

	if (!*dat || !*bin) {
		LOG_ERR("Manifest format unknown");
		return -1;
	}

	return 0;
}

static void enter_dfu_cmd(void)
{
	LOG_INF("Sending command to enter DFU mode: '%s'", conf.dfucmd);
	write(ser_fd, conf.dfucmd, strlen(conf.dfucmd));
	write(ser_fd, "\r\n", 2);
	sleep(1);
	char b[200];
	int ret = read(ser_fd, b, 200);
	if (ret > 0) {
		/* debug output reply */
		b[ret--] = '\0';
		while (b[ret] == '\r' || b[ret] == '\n')
			b[ret--] = '\0';
		LOG_DBG("Device replied: '%s'", b);
	}
}

int main(int argc, char *argv[])
{
	int ret = EXIT_FAILURE;
	char* dat = NULL;
	char* bin = NULL;
	zip_file_t* zf1 = NULL;
	zip_file_t* zf2 = NULL;
	size_t zs1, zs2;

	main_options(argc, argv);

	LOG_INF("Serial Port: %s", conf.serport);
	LOG_INF("DFU Package: %s", conf.zipfile);

	zip_t* zip = zip_open(conf.zipfile, ZIP_RDONLY, NULL);
	if (zip == NULL) {
		LOG_ERR("Could not open ZIP file '%s'", conf.zipfile);
		goto exit;
	}

	ret = read_manifest(zip, &dat, &bin);
	if (ret < 0 || !dat || !bin) {
		goto exit;
	}

	/* open data files in ZIP file */
	zf1 = zip_file_open(zip, dat, &zs1);
	zf2 = zip_file_open(zip, bin, &zs2);
	if (zf1 == NULL || zf2 == NULL) {
		goto exit;
	}

	ser_fd = serial_init(conf.serport);
	if (ser_fd <= 0) {
		goto exit;
	}

	/* first check if Bootloader responds to Ping */
	LOG_NOTI_("Waiting for device to be ready: ");
	do {
		if (conf.debug < LL_INFO) {
			printf("."); fflush(stdout);
		}

		ret = dfu_ping();
		if (!ret) {
			if (conf.dfucmd) {
				enter_dfu_cmd();
			} else {
				sleep(1);
			}
		}
	} while (!ret);

	LOG_NL(LL_NOTICE);
	LOG_NOTI("Starting DFU upgrade");

	/* Upgrade process */
	if (!dfu_set_packet_receive_notification(0))
		goto exit;

	if (!dfu_get_serial_mtu())
		goto exit;

	LOG_NOTI_("Sending Init: ");
	if (!dfu_object_write_procedure(1, zf1, zs1))
		goto exit;
	LOG_NL(LL_NOTICE);

	LOG_NOTI_("Sending Firmware: ");
	if (!dfu_object_write_procedure(2, zf2, zs2))
		goto exit;
	LOG_NL(LL_NOTICE);
	LOG_NOTI("Done");

	ret = EXIT_SUCCESS;

exit:
	free(bin);
	free(dat);
	if (zf1) zip_fclose(zf1);
	if (zf2) zip_fclose(zf2);
	if (zip) zip_close(zip);
	serial_fini(ser_fd);
	return ret;
}
