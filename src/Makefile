default:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPICXX) -g -O0 -fopenmp -fopenmp-targets=x86_64-unknown-linux-gnu -fpic -shared -o libchameleon.so chameleon.cpp commthread.cpp -lffi
	
	cp chameleon.h /home/jk869269/install/chameleon-lib/1.0/include/
	cp libchameleon.so /home/jk869269/install/chameleon-lib/1.0/lib/

clean:
	rm -f *.so *.o
