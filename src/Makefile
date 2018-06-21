default:
	$(MPICXX) -g -O0 -fopenmp -fpic -shared -o libchameleon.so chameleon.cpp -lffi
	@# Target region in shared library is not working that well at the moment
	@#$(MPICC) -g -O0 -fopenmp -fopenmp-targets=x86_64-unknown-linux-gnu -fpic -shared -o libchameleon.so chameleon.c
	cp chameleon.h /home/jk869269/install/chameleon-lib/1.0/include/
	cp libchameleon.so /home/jk869269/install/chameleon-lib/1.0/lib/

clean:
	rm -f *.so *.o
