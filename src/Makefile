INSTALL_DIR=~/chameleon

default: release

debug:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPICXX) -g -O0 -fopenmp -fopenmp-targets=x86_64-unknown-linux-gnu  -std=c++11 -fpic -shared -DCHAM_DEBUG -o libchameleon.so chameleon.cpp commthread.cpp cham_statistics.cpp -lffi -lpthread -lhwloc
	
	cp chameleon.h $(INSTALL_DIR)/include/
	cp libchameleon.so $(INSTALL_DIR)/lib/

release:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPICXX) -O3 -fopenmp -fopenmp-targets=x86_64-unknown-linux-gnu  -std=c++11 -fpic -shared -o libchameleon.so chameleon.cpp commthread.cpp cham_statistics.cpp -lffi -lpthread -lhwloc
	
	cp chameleon.h $(INSTALL_DIR)/include/
	cp libchameleon.so $(INSTALL_DIR)/lib/

trace:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPICXX) -O3 -g -DTRACE -I$(VT_ROOT)/include -fopenmp -fopenmp-targets=x86_64-unknown-linux-gnu  -std=c++11 -fpic -shared -o libchameleon.so chameleon.cpp commthread.cpp cham_statistics.cpp -lffi -lpthread -lhwloc -trace
	
	cp chameleon.h $(INSTALL_DIR)/include/
	cp libchameleon.so $(INSTALL_DIR)/lib/
clean:
	rm -f *.so *.o
