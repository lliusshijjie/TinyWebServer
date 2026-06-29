CXX ?= g++

CXXFLAGS += -std=c++17 -Wall -Wextra

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g -O0
else
    CXXFLAGS += -O2
endif

SRCS = main.cpp ./timer/lst_timer.cpp ./http/http_conn.cpp ./http/user_cache.cpp \
       ./log/log.cpp ./CGImysql/sql_connection_pool.cpp webserver.cpp config.cpp

.PHONY: server clean

server: $(SRCS)
	$(CXX) -o server $^ $(CXXFLAGS) -lpthread -lmysqlclient

clean:
	rm -f server
