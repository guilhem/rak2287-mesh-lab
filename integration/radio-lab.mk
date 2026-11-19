SOCKET_REPEATER := $(BUILD)/socket-repeater/repeater.cpp

$(SOCKET_REPEATER): repeater.cpp $(LAB_ROOT)/integration/meshcore-socket.patch | $(BUILD)
	mkdir -p "$(dir $@)"
	patch --silent -o "$@" "$<" "$(LAB_ROOT)/integration/meshcore-socket.patch"

$(BUILD)/repeater.o: $(SOCKET_REPEATER)
	$(CXX) $(CPPFLAGS) -I"$(LAB_ROOT)/integration" $(CXXFLAGS) -c "$<" -o "$@"

.PHONY: lab-broker lab-check
lab-broker: $(BUILD)/radio-broker

$(BUILD)/radio_broker.o: $(LAB_ROOT)/integration/radio_broker.c | $(BUILD)
	$(CC) $(CPPFLAGS) -I"$(DRIVER)" $(CFLAGS) -std=c11 -Wall -Wextra -Werror -c "$<" -o "$@"
$(BUILD)/broker_scenario.o: $(LAB_ROOT)/integration/tests/broker_scenario.c | $(BUILD)
	$(CC) $(CPPFLAGS) -I"$(DRIVER)" $(CFLAGS) -std=c11 -Wall -Wextra -Werror -c "$<" -o "$@"
$(BUILD)/radio-broker: $(BUILD)/radio_broker.o $(RADIO_LIBS) $(HAL_LIBS)
	$(CC) $(LDFLAGS) -Wl,--gc-sections $^ -lrt -lm -o "$@"
$(BUILD)/radio-sim-broker: $(BUILD)/radio_broker.o $(BUILD)/broker_scenario.o
	$(CC) $(LDFLAGS) $^ -o "$@"

lab-check: check $(BUILD)/radio-sim-broker
	python3 -B "$(LAB_ROOT)/integration/test_radio_broker.py" "$(BUILD)/radio-sim-broker" "$(BUILD)/meshcore-sim-repeater"

-include $(BUILD)/radio_broker.d $(BUILD)/broker_scenario.d
