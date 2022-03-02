HOST_GCC=g++
TARGET_GCC=gcc

PLUGIN_FUNCTION_PROEPILOGUE=function_proepilogue_plugin.c
PLUGIN_FIX_RELOCATIONS=fix_relocations_plugin.c
PLUGIN_RERANDOMIZATION_WRAPPER=rerandomization_wrapper_plugin.c

GCCPLUGINS_DIR:= $(shell $(TARGET_GCC) -print-file-name=plugin)
CXXFLAGS+= -I$(GCCPLUGINS_DIR)/include -fPIC -fno-rtti
#-O2

all: function_proepilogue_plugin.so fix_relocations_plugin.so rerandomization_wrapper_plugin.so

function_proepilogue_plugin.so: $(PLUGIN_FUNCTION_PROEPILOGUE) 
	$(HOST_GCC) -shared $(CXXFLAGS) $^ -o $@

fix_relocations_plugin.so: $(PLUGIN_FIX_RELOCATIONS)
	$(HOST_GCC) -shared $(CXXFLAGS) $^ -o $@

rerandomization_wrapper_plugin.so: $(PLUGIN_RERANDOMIZATION_WRAPPER)
	$(HOST_GCC) -shared $(CXXFLAGS) $^ -o $@


clean:
	rm *.so


