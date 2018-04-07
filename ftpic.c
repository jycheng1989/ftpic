#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include <fftw3.h>

#include <qdsp.h>

#include "common.h"

const int MODELOG_MAX = 64;

// time info
double DT = 0.001;
double TMAX = 20;

// fft plans and buffers
fftw_plan phiIFFT;
fftw_complex *phikBuf;
double *phixBuf;

// USFFT buffers
fftw_complex *zcBuf;
double *xpBuf;
double *fpBuf;

fftw_complex *ekBuf;
fftw_complex *epBuf;

FILE *modeLog = NULL;
FILE *paramLog = NULL;

extern void uf1t_(int*, double*, int*, double*, double*, int*, int*);
extern void uf1a_(int*, double*, int*, double*, double*, int*, int*);

double shape(double x);

void deposit(double *x, fftw_complex *rhok, fftw_complex *sk);
void fields(fftw_complex *rhok, fftw_complex *sk, double *phi, double *potential);
void xPush(double *x, double *v);
void vHalfPush(double *x, double *v, int forward);

double kineticEnergy(double *v);
double momentum(double *v);

int main(int argc, char **argv) {
	DX = XMAX / NGRID;

	// whether to plot
	int phasePlotOn = 1;
	int phiPlotOn = 1;
	int rhoPlotOn = 1;
	
	// parse arguments
	for (int i = 1; i < argc; i++) {
		// log file for parameters
		if (!strcmp(argv[i], "-p")) {
			if (++i == argc) return 1;
			paramLog = fopen(argv[i], "w");
		}

		// log file for modes
		if (!strcmp(argv[i], "-m")) {
			if (++i == argc) return 1;
			modeLog = fopen(argv[i], "w");
		}

		// time step and limit
		if (!strcmp(argv[i], "-t")) {
			if (++i == argc) return 1;
			sscanf(argv[i], "%lf,%lf", &DT, &TMAX);
			if (DT <= 0) return 1;
		}

		// quiet, do not render plots
		if (!strcmp(argv[i], "-q")) {
			phasePlotOn = 0;
			phiPlotOn = 0;
			rhoPlotOn = 0;
		}
	}

	// dump parameters
	if (paramLog) {
		// basic
		fprintf(paramLog, " particles: %i\n", PART_NUM);
		fprintf(paramLog, "  timestep: %e\n", DT);
		fprintf(paramLog, "    length: %e\n", XMAX);
		fprintf(paramLog, "    v_beam: %e\n", BEAM_SPEED);
		fprintf(paramLog, "      mass: %e\n", PART_MASS);
		fprintf(paramLog, "    charge: %e\n", PART_CHARGE);
		fprintf(paramLog, "     eps_0: %e\n", EPS_0);
		fprintf(paramLog, "\n");

		// Debye length
		double kt = PART_MASS * BEAM_SPEED * BEAM_SPEED;
		double dens = PART_NUM / XMAX;
		double ne2 = (dens * PART_CHARGE * PART_CHARGE);
		fprintf(paramLog, "    lambda: %e\n", sqrt(EPS_0 * kt / ne2));

		// plasma frequency
		OMEGA_P = sqrt(ne2 / (PART_MASS * EPS_0));
		fprintf(paramLog, " frequency: %e\n", OMEGA_P);

		fclose(paramLog);
	}

	// header for modes
	if (modeLog) {
		fprintf(modeLog, "time");
		for (int i = 1; i <= MODELOG_MAX; i++)
			fprintf(modeLog, ",m%d", i);
		fprintf(modeLog, "\n");
	}

	// allocate memory
	double *x = malloc(PART_NUM * sizeof(double));
	double *v = malloc(PART_NUM * sizeof(double));
	int *color = malloc(PART_NUM * sizeof(int));
	
	fftw_complex *rhok = fftw_malloc(NGRID * sizeof(fftw_complex));
	double *rhox = fftw_malloc(NGRID * sizeof(double));
	double *phix = fftw_malloc(NGRID * sizeof(double));

	double *sx = fftw_malloc(NGRID * sizeof(double));
	fftw_complex *sk = fftw_malloc(NGRID * sizeof(fftw_complex));

	// transform buffers
	phikBuf = fftw_malloc(NGRID/2 * sizeof(fftw_complex));
	phixBuf = fftw_malloc(NGRID * sizeof(double));

	// USFFT buffers
	zcBuf = malloc(NGRID * sizeof(fftw_complex));
	xpBuf = malloc(PART_NUM * sizeof(double));
	fpBuf = malloc(2*PART_NUM * sizeof(double));

	// reverse USFFT
	ekBuf = malloc(2 * NGRID * sizeof(fftw_complex));
	epBuf = malloc(PART_NUM * sizeof(fftw_complex));
	
	// plan transforms
	fftw_plan rhoIFFT = fftw_plan_dft_c2r_1d(NGRID, rhok, rhox, FFTW_MEASURE);
	phiIFFT = fftw_plan_dft_c2r_1d(NGRID, phikBuf, phixBuf, FFTW_MEASURE);

	// determine s(k)
	fftw_plan sFFT = fftw_plan_dft_r2c_1d(NGRID, sx, sk, FFTW_ESTIMATE);

	double sxsum = 0;
	for (int j = 0; j < NGRID; j++) {
		double xcur = j * XMAX / NGRID;
		sx[j] = shape(xcur) + shape(XMAX - xcur);
		sxsum += sx[j];
	}

	// USFFT coefficients are 1 + 0i
	// change this so we can convolute in USFFT
	for (int m = 0; m < PART_NUM; m++) {
		fpBuf[2*m] = 1;
		fpBuf[2*m + 1] = 0;
	}
	
	sxsum *= DX;
	for (int j = 0; j < NGRID; j++) sx[j] /= sxsum;

	fftw_execute(sFFT);
	fftw_destroy_plan(sFFT);
	
	// initialize particles
	initLandau(x, v, color);
	//init2Stream(x, v, color);

	QDSPplot *phasePlot = NULL;
	QDSPplot *phiPlot = NULL;
	QDSPplot *rhoPlot = NULL;

	if (phasePlotOn) {
		phasePlot = qdspInit("Phase plot");
		qdspSetBounds(phasePlot, 0, XMAX, -30, 30);
		qdspSetGridX(phasePlot, 0, 2, 0x888888);
		qdspSetGridY(phasePlot, 0, 10, 0x888888);
		qdspSetPointColor(phasePlot, 0x000000);
		qdspSetBGColor(phasePlot, 0xffffff);
	}

	if (phiPlotOn) {
		phiPlot = qdspInit("Phi(x)");
		qdspSetBounds(phiPlot, 0, XMAX, -100, 100);
		qdspSetGridX(phiPlot, 0, 2, 0x888888);
		qdspSetGridY(phiPlot, 0, 20, 0x888888);
		qdspSetConnected(phiPlot, 1);
		qdspSetPointColor(phiPlot, 0x000000);
		qdspSetBGColor(phiPlot, 0xffffff);
	}

	if (rhoPlotOn) {
		rhoPlot = qdspInit("Rho(x)");
		qdspSetBounds(rhoPlot, 0, XMAX, -50, 50);
		qdspSetGridX(rhoPlot, 0, 2, 0x888888);
		qdspSetGridY(rhoPlot, 0, 10, 0x888888);
		qdspSetConnected(rhoPlot, 1);
		qdspSetPointColor(rhoPlot, 0x000000);
		qdspSetBGColor(rhoPlot, 0xffffff);
	}
	
	double *xar = malloc(NGRID * sizeof(double));
	for (int j = 0; j < NGRID; j++) xar[j] = j * DX;

	double potential;
	
	deposit(x, rhok, sk);
	fields(rhok, sk, phix, &potential);
	
	vHalfPush(x, v, 0);

	int open = 1;

	printf("time,potential,kinetic,total,momentum\n");

	double minp = 1/0.0;
	double maxp = 0.0;
	
	for (int n = 0; open && n * DT < TMAX; n++) {
		if (modeLog) fprintf(modeLog, "%f", n * DT);

		deposit(x, rhok, sk);
		fields(rhok, sk, phix, &potential);
		vHalfPush(x, v, 1);

		if (phasePlotOn)
			open = qdspUpdateIfReady(phasePlot, x, v, color, PART_NUM);

		// logging
		if (n % 10 == 0) {
			double kinetic = kineticEnergy(v);
			double curp = momentum(v);
			printf("%f,%f,%f,%f,%f\n",
			       n * DT,
			       potential,
			       kinetic,
			       potential + kinetic,
			       curp);
			if (curp < minp) minp = curp;
			if (curp > maxp) maxp = curp;
		}

		if (phiPlotOn)
			phiPlotOn = qdspUpdateIfReady(phiPlot, xar, phix, NULL, NGRID);
		
		if (rhoPlotOn) {
			fftw_execute(rhoIFFT);
			rhoPlotOn = qdspUpdateIfReady(rhoPlot, xar, rhox, NULL, NGRID);
		}

		vHalfPush(x, v, 1);
		xPush(x, v);
	}

	// cleanup
	if(modeLog) fclose(modeLog);

	free(x);
	free(v);
	free(color);

	free(zcBuf);
	free(xpBuf);
	free(fpBuf);
	
	fftw_free(rhok);
	fftw_free(rhox);
	fftw_free(phix);

	fftw_free(sx);
	fftw_free(sk);

	fftw_destroy_plan(phiIFFT);
	fftw_destroy_plan(rhoIFFT);

	if (phasePlot) qdspDelete(phasePlot);
	if (phiPlot) qdspDelete(phiPlot);
	if (rhoPlot) qdspDelete(rhoPlot);

	return 0;
}

// particle shape function, centered at 0, gaussian in this case
double shape(double x) {
	//const double sigma = 0.05;
	//return exp(-x*x / (2 * sigma * sigma)) / sqrt(2 * M_PI * sigma * sigma);
	return 1.0 * (x == 0);
	//return fmax(1 - fabs(x/DX), 0);
}

// determines rho(k) from list of particle positions
void deposit(double *x, fftw_complex *rhok, fftw_complex *sk) {

	int nc = NGRID;
	int np = PART_NUM;
	int isign = -1;
	int order = 5;

	#pragma omp parallel for
	for (int m = 0; m < PART_NUM; m++) {
		xpBuf[m] = x[m] / XMAX;
		//if (xpBuf[m] >= 0.5) xpBuf[m] -= 1.0;
	}
	
	uf1t_(&nc, (double*)zcBuf, &np, xpBuf, fpBuf, &isign, &order);

	//#pragma omp parallel for
	for (int j = 0; j < NGRID/2; j++) {
		double real = PART_CHARGE * zcBuf[NGRID/2 + j][0] / NGRID;
		double imag = PART_CHARGE * zcBuf[NGRID/2 + j][1] / NGRID;
		//printf("%d\t%f,%f\t%f,%f\n", j, real, imag);
		rhok[j][0] = real * sk[j][0] - imag * sk[j][1];
		rhok[j][1] = real * sk[j][1] + imag * sk[j][0];
	}

	// background
	rhok[0][0] = 0;
}

// determine phi and e from rho(k) 
void fields(fftw_complex *rhok, fftw_complex *sk, double *phi, double *potential) {
	
	// rho(k) -> phi(k)
	phikBuf[0][0] = 0;
	phikBuf[0][1] = 0;
	for (int j = 1; j < NGRID/2; j++) {
		double k = 2 * M_PI * j / XMAX;
		
		double phikRe = rhok[j][0] / (k * k * EPS_0);
		double phikIm = rhok[j][1] / (k * k * EPS_0);

		phikBuf[j][0] = (sk[j][0] * phikRe - sk[j][1] * phikIm) * DX;
		phikBuf[j][1] = (sk[j][0] * phikIm + sk[j][1] * phikRe) * DX;

		ekBuf[NGRID/2 + j][0] = k * phikBuf[j][1];
		ekBuf[NGRID/2 + j][1] = -k * phikBuf[j][0];
	}

	// find PE
	if (potential != NULL) {
		double pot = 0;
		for (int j = 1; j < NGRID / 2; j++) {
			pot += phikBuf[j][0] * rhok[j][0] + phikBuf[j][1] * rhok[j][1];
		}
		*potential = pot * XMAX;

		if (modeLog) {
			for (int j = 1; j <= MODELOG_MAX; j++) {
				double etmp = phikBuf[j][0] * rhok[j][0]
					+ phikBuf[j][1] * rhok[j][1];
				fprintf(modeLog, ",%e", etmp);
			}
			fprintf(modeLog, "\n");
		}
	}

	// phi(k) -> phi(x)
	fftw_execute(phiIFFT);
	memcpy(phi, phixBuf, NGRID * sizeof(double));
}

// moves particles given velocities
void xPush(double *x, double *v) {
#pragma omp parallel for
	for (int i = 0; i < PART_NUM; i++) {
		x[i] += DT * v[i];

		// periodicity
		// (not strictly correct, but if a particle is moving several grid
		// lengths in 1 timestep, something has gone horribly wrong)
		if (x[i] < 0) x[i] += XMAX;
		if (x[i] >= XMAX) x[i] -= XMAX;
	}
}

// interpolates E field on particles and accelerates them 1/2 timestep
void vHalfPush(double *x, double *v, int forward) {
	// calculate forces from E(k)
	int nc = NGRID;
	int np = PART_NUM;
	int isign = 1;
	int order = 5;

	// first element of ekBuf must be 0
	ekBuf[0][0] = 0;
	ekBuf[0][1] = 0;
	for (int j = 0; j < NGRID/2; j++) {
		// array must be Hermitian
		ekBuf[NGRID/2 - j][0] = ekBuf[j + NGRID/2][0];
		ekBuf[NGRID/2 - j][1] = -ekBuf[j + NGRID/2][1];
	}
	
#pragma omp parallel for
	for (int m = 0; m < PART_NUM; m++) {
		xpBuf[m] = x[m] / XMAX;
	}
	
	uf1a_(&nc, (double*)ekBuf, &np, xpBuf, (double*)epBuf, &isign, &order);
	for (int m = 0; m < PART_NUM; m++) {
		// interpolated e(x_m)
		double ePart = epBuf[m][0];

		// push
		if (forward)
			v[m] += DT/2 * (PART_CHARGE / PART_MASS) * ePart;
		else
			v[m] -= DT/2 * (PART_CHARGE / PART_MASS) * ePart;
	}
}

double kineticEnergy(double *v) {
	double kinetic = 0;
	
#pragma omp parallel for reduction(+:kinetic)
	for (int i = 0; i < PART_NUM; i++) {
		kinetic += v[i] * v[i] * PART_MASS / 2;
	}
	
	return kinetic;
}

double momentum(double *v) {
	double p = 0;

	#pragma omp parallel for reduction(+:p)
	for (int i = 0; i < PART_NUM; i++) {
		p += PART_MASS * v[i];
	}

	return p;
}
