  
CXX ?= g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
	CXXFLAGS += '-std=c++11'
else
    CXXFLAGS += -O2
	
endif

server: main.cpp http_conn.cpp lst_timer.h log.cpp sql_connection_pool.cpp
	$(CXX) -o server  $^ $(CXXFLAGS) -lpthread -L/usr/lib/x86_64-linux-gnu -lmysqlclient

clean:
	rm  -r server