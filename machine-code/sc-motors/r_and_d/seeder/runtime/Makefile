CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra

# sFoundation SDK paths (installed on Pi)
SFOUNDATION_INC = $(HOME)/teknic/Linux_Software/sFoundation/inc/inc-pub
SFOUNDATION_LIB = -lsFoundation20

# Project paths
INC_DIR = include
SRC_DIR = src

INCLUDES = -I$(INC_DIR) -I$(INC_DIR)/utils -I$(INC_DIR)/seeder -I$(SFOUNDATION_INC)

SRCS = $(SRC_DIR)/main.cpp \
       $(SRC_DIR)/seeder/SeederApp.cpp \
       $(SRC_DIR)/utils/ClearCoreClient.cpp \
       $(SRC_DIR)/utils/Axis.cpp \
       $(SRC_DIR)/utils/JsonlTelemetryLogger.cpp \
       $(SRC_DIR)/utils/SafetySupervisor.cpp \
       $(SRC_DIR)/utils/TouchEncoderState.cpp \
       $(SRC_DIR)/utils/Input.cpp

TARGET = seeder_test

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRCS) $(SFOUNDATION_LIB) -lpthread -o $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
