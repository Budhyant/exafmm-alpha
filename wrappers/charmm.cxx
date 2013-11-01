#include "localessentialtree.h"
#include "args.h"
#include "boundbox.h"
#include "buildtree.h"
#include "ewald.h"
#include "sort.h"
#include "traversal.h"
#include "updownpass.h"
#include "vanderwaals.h"

Args *args;
Logger *logger;
Sort *sort;
Bounds localBounds;
BoundBox *boundbox;
BuildTree *tree;
UpDownPass *pass;
Traversal *traversal;
LocalEssentialTree *LET;

extern "C" void fmm_init_(int & images) {
  const int ncrit = 32;
  const int nspawn = 1000;
  const real_t theta = 0.4;
  args = new Args;
  logger = new Logger;
  sort = new Sort;
  boundbox = new BoundBox(nspawn);
  tree = new BuildTree(ncrit, nspawn);
  pass = new UpDownPass(theta);
  traversal = new Traversal(nspawn, images);
  LET = new LocalEssentialTree(images);

  args->theta = theta;
  args->ncrit = ncrit;
  args->nspawn = nspawn;
  args->images = images;
  args->mutual = 0;
  args->verbose = 1;
  args->distribution = "external";
  args->verbose &= LET->mpirank == 0;
  if (args->verbose) {
    logger->verbose = true;
    boundbox->verbose = true;
    tree->verbose = true;
    pass->verbose = true;
    traversal->verbose = true;
    LET->verbose = true;
  }
  logger->printTitle("Initial Parameters");
  args->print(logger->stringLength, P, LET->mpirank);
}

extern "C" void fmm_partition_(int & nglobal, int * icpumap, double * x, double * q, double & cycle) {
  logger->printTitle("Partition Profiling");
  int nlocal = 0;
  for (int i=0; i<nglobal; i++) {
    if (icpumap[i] == 1) nlocal++;
  }
  Bodies bodies(nlocal);
  B_iter B = bodies.begin();
  for (int i=0; i<nglobal; i++) {
    if (icpumap[i] == 1) {
      B->X[0] = x[3*i+0];
      B->X[1] = x[3*i+1];
      B->X[2] = x[3*i+2];
      boundbox->restrictToCycle(B->X, cycle);
      B->SRC = q[i];
      B->IBODY = i;
      B++;
    }
  }
  localBounds = boundbox->getBounds(bodies);
  Bounds globalBounds = LET->allreduceBounds(localBounds);
  localBounds = LET->partition(bodies,globalBounds);
  bodies = sort->sortBodies(bodies);
  bodies = LET->commBodies(bodies);
  Cells cells = tree->buildTree(bodies, localBounds);
  pass->upwardPass(cells);
  for (int i=0; i<nglobal; i++) {
    icpumap[i] = 0;
  }
  for (B_iter B=bodies.begin(); B!=bodies.end(); B++) {
    int i = B->IBODY;
    x[3*i+0] = B->X[0];
    x[3*i+1] = B->X[1];
    x[3*i+2] = B->X[2];
    q[i]     = B->SRC;
    icpumap[i] = 1;
  }
}

extern "C" void fmm_coulomb_(int & nglobal, int * icpumap,
			     double * x, double * q, double * p, double * f,
			     double & cycle) {
  int nlocal = 0;
  for (int i=0; i<nglobal; i++) {
    if (icpumap[i] == 1) nlocal++;
  }
  args->numBodies = nlocal;
  logger->printTitle("FMM Parameters");
  args->print(logger->stringLength, P, LET->mpirank);
#if _OPENMP
#pragma omp parallel
#pragma omp master
#endif
  logger->printTitle("FMM Profiling");
  logger->startTimer("Total FMM");
  logger->startPAPI();
  Bodies bodies(nlocal);
  B_iter B = bodies.begin();
  for (int i=0; i<nglobal; i++) {
    if (icpumap[i] == 1) {
      B->X[0] = x[3*i+0];
      B->X[1] = x[3*i+1];
      B->X[2] = x[3*i+2];
      boundbox->restrictToCycle(B->X, cycle);
      B->SRC = q[i];
      B->TRG[0] = p[i];
      B->TRG[1] = f[3*i+0];
      B->TRG[2] = f[3*i+1];
      B->TRG[3] = f[3*i+2];
      B->IBODY = i;
      B++;
    }
  }
  Cells cells = tree->buildTree(bodies, localBounds);
  pass->upwardPass(cells);
  LET->setLET(cells, localBounds, cycle);
  LET->commBodies();
  LET->commCells();
  traversal->dualTreeTraversal(cells, cells, cycle, args->mutual);
  Cells jcells;
  for (int irank=1; irank<LET->mpisize; irank++) {
    LET->getLET(jcells,(LET->mpirank+irank)%LET->mpisize);
    traversal->dualTreeTraversal(cells, jcells, cycle);
  }
  pass->downwardPass(cells);
  vec3 localDipole = pass->getDipole(bodies,0);
  vec3 globalDipole = LET->allreduceVec3(localDipole);
  int numBodies = LET->allreduceInt(bodies.size());
  pass->dipoleCorrection(bodies, globalDipole, numBodies, cycle);
  logger->stopPAPI();
  logger->stopTimer("Total FMM");
  logger->printTitle("Total runtime");
  logger->printTime("Total FMM");

  for (B_iter B=bodies.begin(); B!=bodies.end(); B++) {
    int i = B->IBODY;
    p[i]     = B->TRG[0];
    f[3*i+0] = B->TRG[1];
    f[3*i+1] = B->TRG[2];
    f[3*i+2] = B->TRG[3];
  }
}

extern "C" void fmm_coulomb_exclusion_(int & nglobal, int * icpumap,
				       double * x, double * q, double * p, double * f,
				       double & cycle, int * numex, int * natex) {
  logger->startTimer("Coulomb Exclusion");
  for (int i=0, ic=0; i<nglobal; i++) {
    if (icpumap[i] == 1) {
      for (int jc=0; jc<numex[i]; jc++, ic++) {
	int j = natex[ic]-1;
	vec3 dX;
	for (int d=0; d<3; d++) dX[d] = x[3*i+d] - x[3*j+d];
        boundbox->restrictToCycle(dX, cycle);
	float R2 = norm(dX);
	float invR = 1 / std::sqrt(R2);
	if (R2 == 0) invR = 0;
	float invR3 = q[j] * invR * invR * invR;
	p[i] -= q[j] * invR;
	f[3*i+0] += dX[0] * invR3;
	f[3*i+1] += dX[1] * invR3;
	f[3*i+2] += dX[2] * invR3;
      }
    } else {
      ic += numex[i];
    }
  }
  logger->stopTimer("Coulomb Exclusion");
}

extern "C" void fmm_vanderwaals_(int & nglobal, int * icpumap, int * atype,
				 double * x, double * q, double * p, double * f,
				 double & cuton, double & cutoff, double & cycle,
				 int & nat, double * rscale, double * gscale,
				 double * frscale, double * fgscale) {
  VanDerWaals * VDW = new VanDerWaals(cuton, cutoff, cycle, nat, rscale, gscale, frscale, fgscale);
  int nlocal = 0;
  for (int i=0; i<nglobal; i++) {
    if (icpumap[i] == 1) nlocal++;
  }
  args->numBodies = nlocal;
  logger->printTitle("Van Der Waals Parameters");
  args->print(logger->stringLength, P, LET->mpirank);
#if _OPENMP
#pragma omp parallel
#pragma omp master
#endif
  logger->printTitle("Van Der Waals Profiling");
  logger->startTimer("Van Der Waals");
  logger->startPAPI();
  Bodies bodies(nlocal);
  B_iter B = bodies.begin();
  for (int i=0; i<nglobal; i++) {
    if (icpumap[i] == 1) {
      B->X[0] = x[3*i+0];
      B->X[1] = x[3*i+1];
      B->X[2] = x[3*i+2];
      boundbox->restrictToCycle(B->X, cycle);
      B->SRC = atype[i]+ .5;
      B->TRG[0] = p[i];
      B->TRG[1] = f[3*i+0];
      B->TRG[2] = f[3*i+1];
      B->TRG[3] = f[3*i+2];
      B->IBODY = i;
      B++;
    }
  }
  Cells cells = tree->buildTree(bodies, localBounds);
  pass->upwardPass(cells);
  LET->setLET(cells, localBounds, cycle);
  LET->commBodies();
  LET->commCells();
  VDW->evaluate(cells, cells);
  Cells jcells;
  for (int irank=1; irank<LET->mpisize; irank++) {
    LET->getLET(jcells,(LET->mpirank+irank)%LET->mpisize);
    VDW->evaluate(cells, jcells);
  }
  logger->stopPAPI();
  logger->stopTimer("Van Der Waals");
  logger->printTitle("Total runtime");
  logger->printTime("Van Der Waals");

  for (B_iter B=bodies.begin(); B!=bodies.end(); B++) {
    int i = B->IBODY;
    p[i]     = B->TRG[0];
    f[3*i+0] = B->TRG[1];
    f[3*i+1] = B->TRG[2];
    f[3*i+2] = B->TRG[3];
  }
  delete VDW;
}

extern "C" void fmm_vanderwaals_exclusion_(int & nglobal, int * icpumap,
					   double * x, double * q, double * p, double * f,
					   double & cycle, int * numex, int * natex) {
  logger->startTimer("Van Der Waals Exclusion");
  for (int i=0, ic=0; i<nglobal; i++) {
    if (icpumap[i] == 1) {
      for (int jc=0; jc<numex[i]; jc++, ic++) {
	int j = natex[ic]-1;
	vec3 dX;
	for (int d=0; d<3; d++) dX[d] = x[3*i+d] - x[3*j+d];
        boundbox->restrictToCycle(dX, cycle);
	float R2 = norm(dX);
	float invR = 1 / std::sqrt(R2);
	if (R2 == 0) invR = 0;
	float invR3 = q[j] * invR * invR * invR;
	p[i] -= q[j] * invR;
	f[3*i+0] += dX[0] * invR3;
	f[3*i+1] += dX[1] * invR3;
	f[3*i+2] += dX[2] * invR3;
      }
    } else {
      ic += numex[i];
    }
  }
  logger->stopTimer("Van Der Waals Exclusion");
}

extern "C" void ewald_coulomb_(int & nglobal, int * icpumap, double * x, double * q, double * p, double * f,
			       int & ksize, double & alpha, double & sigma, double & cutoff, double & cycle) {
  Ewald * ewald = new Ewald(ksize, alpha, sigma, cutoff, cycle);
  if (args->verbose) ewald->verbose = true;
  int nlocal = 0;
  for (int i=0; i<nglobal; i++) {
    if (icpumap[i] == 1) nlocal++;
  }
  args->numBodies = nlocal;
  logger->printTitle("Ewald Parameters");
  args->print(logger->stringLength, P, LET->mpirank);
  ewald->print(logger->stringLength);
#if _OPENMP
#pragma omp parallel
#pragma omp master
#endif
  logger->printTitle("Ewald Profiling");
  logger->startTimer("Total Ewald");
  logger->startPAPI();
  Bodies bodies(nlocal);
  B_iter B = bodies.begin();
  for (int i=0; i<nglobal; i++) {
    if (icpumap[i] == 1) {
      B->X[0] = x[3*i+0];
      B->X[1] = x[3*i+1];
      B->X[2] = x[3*i+2];
      boundbox->restrictToCycle(B->X, cycle);
      B->SRC = q[i];
      B->TRG[0] = p[i];
      B->TRG[1] = f[3*i+0];
      B->TRG[2] = f[3*i+1];
      B->TRG[3] = f[3*i+2];
      B->IBODY = i;
      B++;
    }
  }
  Cells cells = tree->buildTree(bodies, localBounds);
  Bodies jbodies = bodies;
  for (int i=0; i<LET->mpisize; i++) {
    if (args->verbose) std::cout << "Ewald loop           : " << i+1 << "/" << LET->mpisize << std::endl;
    LET->shiftBodies(jbodies);
    localBounds = boundbox->getBounds(jbodies);
    Cells jcells = tree->buildTree(jbodies, localBounds);
    ewald->wavePart(bodies, jbodies);
    ewald->realPart(cells, jcells);
  }
  ewald->selfTerm(bodies);
  logger->stopPAPI();
  logger->stopTimer("Total Ewald");
  logger->printTitle("Total runtime");
  logger->printTime("Total Ewald");

  for (B_iter B=bodies.begin(); B!=bodies.end(); B++) {
    int i = B->IBODY;
    p[i]     = B->TRG[0];
    f[3*i+0] = B->TRG[1];
    f[3*i+1] = B->TRG[2];
    f[3*i+2] = B->TRG[3];
  }
  delete ewald;
}

extern "C" void direct_coulomb_(int & nglobal, int * icpumap, double * x, double * q, double * p, double * f, double & cycle) {
  int images = args->images;
  int prange = 0;
  for (int i=0; i<images; i++) {
    prange += int(std::pow(3.,i));
  }
  double Xperiodic[3];
  for (int i=0; i<nglobal; i++) {
    if (icpumap[i] == 1) {
      double pp = 0, fx = 0, fy = 0, fz = 0;
      for (int ix=-prange; ix<=prange; ix++) {
	for (int iy=-prange; iy<=prange; iy++) {
	  for (int iz=-prange; iz<=prange; iz++) {
	    Xperiodic[0] = ix * cycle;
	    Xperiodic[1] = iy * cycle;
	    Xperiodic[2] = iz * cycle;
	    for (int j=0; j<nglobal; j++) {
	      double dx = x[3*i+0] - x[3*j+0] - Xperiodic[0];
	      double dy = x[3*i+1] - x[3*j+1] - Xperiodic[1];
	      double dz = x[3*i+2] - x[3*j+2] - Xperiodic[2];
	      double R2 = dx * dx + dy * dy + dz * dz;
	      double invR = 1 / std::sqrt(R2);
	      if (R2 == 0) invR = 0;
	      double invR3 = q[j] * invR * invR * invR;
	      pp += q[j] * invR;
	      fx += dx * invR3;
	      fy += dy * invR3;
	      fz += dz * invR3;
	    }
	  }
	}
      }
      p[i] += pp;
      f[3*i+0] -= fx;
      f[3*i+1] -= fy;
      f[3*i+2] -= fz;
    }
  }
  double dipole[3] = {0, 0, 0};
  for (int i=0; i<nglobal; i++) {
    for (int d=0; d<3; d++) dipole[d] += x[3*i+d] * q[i];
  }
  double norm = 0;
  for (int d=0; d<3; d++) {
    norm += dipole[d] * dipole[d];
  }
  double coef = 4 * M_PI / (3 * cycle * cycle * cycle);
  for (int i=0; i<nglobal; i++) {
    if (icpumap[i] == 1) {
      p[i] -= coef * norm / nglobal / q[i];
      f[3*i+0] -= coef * dipole[0];
      f[3*i+1] -= coef * dipole[1];
      f[3*i+2] -= coef * dipole[2];
    }
  }
}
