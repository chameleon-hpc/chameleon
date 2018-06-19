default:
	mpicc -fopenmp -fpic -shared -o libchameleon.so chameleon.c
	cp chameleon.h /home/jk869269/install/chameleon-lib/1.0/include/
	cp libchameleon.so /home/jk869269/install/chameleon-lib/1.0/lib/

clean:
	rm -f *.so *.o
