CC      = gcc
CFLAGS  = -Wall -Wextra -g \
          -Istubs \
          -I../../src \
          -I../../src/qemu \
          -I../../src/common
LDFLAGS = -lpthread

DISPATCH_SRCS = test_dispatch.c \
                ../../src/qemu/nvkvm_objects.c \
                ../../src/qemu/nvkvm_dispatch.c \
                ../../src/qemu/nvkvm_handle.c \
                frontend_stubs.c

FRONTEND_SRCS = test_frontend.c \
                ../../src/qemu/nvkvm_frontend.c \
                ../../src/qemu/nvkvm_objects.c

HANDLE_SRCS   = test_handle.c \
                ../../src/qemu/nvkvm_handle.c

ISOLATE_SRCS  = test_isolate.c \
                ../../src/qemu/nvkvm_isolate.c \
                ../../src/qemu/nvkvm_handle.c

TABLES_SRCS   = test_tables.c \
                ../../src/qemu/nvkvm_tables.c

OPEN_SCM_SRCS = test_open_scm.c

.PHONY: all clean run

all: test_dispatch test_frontend test_handle mock_stub test_isolate test_tables test_open_scm

test_dispatch: $(DISPATCH_SRCS)
	$(CC) $(CFLAGS) -o $@ $(DISPATCH_SRCS) $(LDFLAGS)

test_frontend: $(FRONTEND_SRCS)
	$(CC) $(CFLAGS) -o $@ $(FRONTEND_SRCS) $(LDFLAGS)

test_handle: $(HANDLE_SRCS)
	$(CC) $(CFLAGS) -o $@ $(HANDLE_SRCS) $(LDFLAGS)

mock_stub: mock_stub.c
	$(CC) $(CFLAGS) -o $@ mock_stub.c

test_isolate: $(ISOLATE_SRCS) mock_stub
	$(CC) $(CFLAGS) -o $@ $(ISOLATE_SRCS) $(LDFLAGS)

test_tables: $(TABLES_SRCS)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DNVKVM_TABLES_NO_KVM -o $@ $(TABLES_SRCS) $(LDFLAGS)

test_open_scm: $(OPEN_SCM_SRCS)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ $(OPEN_SCM_SRCS) $(LDFLAGS)

run: test_dispatch test_frontend test_handle test_isolate test_tables test_open_scm
	./test_dispatch
	./test_frontend
	./test_handle
	./test_isolate
	./test_tables
	./test_open_scm

clean:
	rm -f test_dispatch test_frontend test_handle mock_stub test_isolate test_tables test_open_scm
