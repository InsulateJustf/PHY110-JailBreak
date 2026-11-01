/* rootdclient — talk to the plant's rootd socket: 4B len | cmd -> 4B rc | 4B len | out */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct sockaddr_un a;
	char buf[1 << 20];
	uint32_t len, rc, olen;
	const char *cmd = argc > 1 ? argv[1] : "id";
	int fd;

	if (argc > 1 && !strcmp(argv[1], "-c") && argc > 2)
		cmd = argv[2];
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) { perror("socket"); return 1; }
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strncpy(a.sun_path, "/data/local/tmp/t98260-root.sock", sizeof(a.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a))) { perror("connect"); return 2; }
	len = (uint32_t)strlen(cmd);
	if (write(fd, &len, 4) != 4 || write(fd, cmd, len) != (ssize_t)len) return 3;
	if (read(fd, &rc, 4) != 4 || read(fd, &olen, 4) != 4) return 4;
	if (olen > sizeof(buf)) olen = sizeof(buf);
	if (olen && read(fd, buf, olen) != (ssize_t)olen) return 5;
	if (olen) fwrite(buf, 1, olen, stdout);
	fprintf(stderr, "[rc=%u]\n", rc);
	return rc ? 6 : 0;
}
