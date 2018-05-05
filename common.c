// this file contains common functions used by both the reference PIC
// implementation and Particle in Fourier
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>

#include "common.h"

// time info
double DT = 0.001;
double TMAX = 20;

const double XMAX = 16.0; // system length
const int NGRID = 64; // grid size
double DX;

// particle number and properties
const int PART_NUM = 10000;
const double PART_MASS = 0.005;
const double PART_CHARGE = -0.02;
const double EPS_0 = 1.0;

double OMEGA_P;

const double BEAM_SPEED = 8.0;
const double V_TH = 3.5; // sqrt(kt/m)

// wave periods per system length for standing wave and landau damping
const int WAVE_MODE = 2;
const double PERTURB_AMPL = 0.25;

// logging
const int MODELOG_MAX = 32;
FILE *modeLog = NULL;

int commonInit(int argc, char **argv,
               double *x, double *v, int *color,
               int *quiet) {
	FILE *paramLog = NULL;
	
	// whether to plot
	*quiet = 0;

	// which test case?
	int initType = 0;
	
	// parse arguments
	for (int i = 1; i < argc; i++) {
		// test case type
		if (!strcmp(argv[i], "-c")) {
			if (++i == argc) return 1;

			if (!strcmp(argv[i], "2stream")) initType = 1;
			else if (!strcmp(argv[i], "landau")) initType = 2;
			else if (!strcmp(argv[i], "standing")) initType = 3;
		}
		
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
			*quiet = 1;
		}
	}

	// initialize particles
	if (initType == 0) {
		if (paramLog != NULL) fclose(paramLog);
		if (modeLog != NULL) fclose(modeLog);
		return 1;
	} else if (initType == 1) {
		init2Stream(x, v, color);
	} else if (initType == 2) {
		initLandau(x, v, color);
	} else if (initType == 3) {
		initStanding(x, v, color);
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
		double kt;
		if (initType == 1) kt = PART_MASS * BEAM_SPEED * BEAM_SPEED;
		else kt = PART_MASS * V_TH * V_TH;
		
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

	return 1;
}

// 2-stream instability, standard test case
void init2Stream(double *x, double *v, int *color) {
	double stddev = sqrt(500 / (5.1e5));
	for (int i = 0; i < PART_NUM; i++) {
		x[i] = i * XMAX / PART_NUM;

		if (i % 2) {
			v[i] = BEAM_SPEED;
			color[i] = 0xff0000;
		} else {
			v[i] = -BEAM_SPEED;
			color[i] = 0x0000ff;
		}

		// box-mueller
		double r1 = (rand() + 1) / ((double)RAND_MAX + 1); // log(0) breaks stuff
		double r2 = (rand() + 1) / ((double)RAND_MAX + 1);
		v[i] += stddev * sqrt(-2 * log(r1)) * cos(2 * M_PI * r2);
	}
}

// helper function for initLandau
// use newton's method to find inverse of position CDF
static double newton(double u) {
	double k = WAVE_MODE * 2*M_PI/XMAX;
	
	// find root of x/L + a/(k L) sin(k x) - u = 0
	// derivative is 1/L + a/L cos(k x)
	double x = u * XMAX;
	double f = x/XMAX + PERTURB_AMPL / (k * XMAX) * sin(k * x) - u;

	while (fabs(f) > 1e-9) {
		double fprime = 1/XMAX + PERTURB_AMPL / XMAX * cos(k * x);
		x -= f / fprime;
		f = x/XMAX + PERTURB_AMPL / (k * XMAX) * sin(k * x) - u;
	}

	return x;
}

// wave in maxwellian, used to test Landau damping
// distribution function: f(x,v) = exp(-1/2 (v/v_th)^2) / (sqrt(2 Pi) * v_th)
//                                 * (1 + a cos(k x)) / L
void initLandau(double *x, double *v, int *color) {
	for (int i = 0; i < PART_NUM; i++) {
		// charge distribution produced via inverse transform sampling
		x[i] = newton(i / (double)PART_NUM);

		// init maxwellian distro
		// box-mueller
		double r1 = (rand() + 1) / ((double)RAND_MAX + 1); // log(0) breaks stuff
		double r2 = (rand() + 1) / ((double)RAND_MAX + 1);
		v[i] = V_TH * sqrt(-2 * log(r1)) * cos(2 * M_PI * r2);
		
		color[i] = 0x0000ff;
	}
}

// standing wave, displaced charges. this should oscillate as a single mode, but
// this doesn't happen in standard PIC. see section 4 of:
// Huang, et al, 2016. Finite grid instability and spectral fidelity of the
// electrostatic Particle-In-Cell algorithm. Computer Physics Communications
// 207, 123–135.
void initStanding(double *x, double *v, int *color) {
	OMEGA_P = sqrt((PART_NUM/XMAX) * (PART_CHARGE*PART_CHARGE)
	               / (PART_MASS * EPS_0));
	
	double ampl = 0.3;
	
	for (int i = 0; i < PART_NUM; i++) {
		x[i] = i * XMAX / PART_NUM;
		x[i] += ampl * XMAX  / (2 * M_PI * WAVE_MODE)
			* cos(2 * M_PI * WAVE_MODE * x[i] / XMAX);

		v[i] = ampl * OMEGA_P * XMAX / (2 * M_PI * WAVE_MODE)
			* sin(2 * M_PI * WAVE_MODE * x[i] / XMAX);

		color[i] = 0x0000ff;
	}
}
