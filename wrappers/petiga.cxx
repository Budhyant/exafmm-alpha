#include "base_mpi.h"
#include "args.h"
#include "bound_box.h"
#include "build_tree.h"
#include "partition.h"
#include "traversal.h"
#include "tree_mpi.h"
#include "up_down_pass.h"

Args *args;
BaseMPI *baseMPI;
BoundBox *boundbox;
BuildTree *build;
Logger *logger;
Partition *partition;
Traversal *traversal;
TreeMPI *treeMPI;
UpDownPass *upDownPass;

Bounds localBounds;

extern "C" void FMM_Init() {
  const int ncrit = 16;
  const int nspawn = 1000;
  const int images = 0;
  const real_t theta = 0.4;
  args = new Args;
  baseMPI = new BaseMPI;
  boundbox = new BoundBox(nspawn);
  build = new BuildTree(ncrit, nspawn);
  logger = new Logger;
  partition = new Partition;
  traversal = new Traversal(nspawn, images);
  treeMPI = new TreeMPI(images);
  upDownPass = new UpDownPass(theta);

  args->theta = theta;
  args->ncrit = ncrit;
  args->nspawn = nspawn;
  args->images = images;
  args->mutual = 0;
  args->verbose = 1;
  args->distribution = "external";
  args->verbose &= baseMPI->mpirank == 0;
  if (args->verbose) {
    logger->verbose = true;
    boundbox->verbose = true;
    build->verbose = true;
    upDownPass->verbose = true;
    traversal->verbose = true;
    treeMPI->verbose = true;
  }
  logger->printTitle("Initial Parameters");
  args->print(logger->stringLength, P, baseMPI->mpirank);
}

extern "C" void FMM_Finalize() {
  delete args;
  delete baseMPI;
  delete boundbox;
  delete build;
  delete logger;
  delete partition;
  delete traversal;
  delete treeMPI;
  delete upDownPass;
}

extern "C" void FMM_Partition(int & ni, double * xi, double * yi, double * zi, double * vi,
			      int & nj, double * xj, double * yj, double * zj, double * vj) {
  logger->printTitle("Partition Profiling");
  Bodies bodies(ni);
  for (B_iter B=bodies.begin(); B!=bodies.end(); B++) {
    int i = B-bodies.begin();
    B->X[0] = xi[i];
    B->X[1] = yi[i];
    B->X[2] = zi[i];
    B->SRC  = vi[i];
  }
  Bodies jbodies(nj);
  for (B_iter B=jbodies.begin(); B!=jbodies.end(); B++) {
    int i = B-jbodies.begin();
    B->X[0] = xj[i];
    B->X[1] = yj[i];
    B->X[2] = zj[i];
    B->SRC  = vj[i];
  }
  localBounds = boundbox->getBounds(bodies);
  localBounds = boundbox->getBounds(jbodies,localBounds);
  Bounds globalBounds = baseMPI->allreduceBounds(localBounds);
  localBounds = partition->partition(bodies,globalBounds);
  bodies = treeMPI->commBodies(bodies);
  partition->partition(jbodies,globalBounds);
  jbodies = treeMPI->commBodies(jbodies);
  Cells cells = build->buildTree(bodies, localBounds);
  upDownPass->upwardPass(cells);
  Cells jcells = build->buildTree(jbodies, localBounds);
  upDownPass->upwardPass(jcells);

  ni = bodies.size();
  for (B_iter B=bodies.begin(); B!=bodies.end(); B++) {
    int i = B-bodies.begin();
    xi[i] = B->X[0];
    yi[i] = B->X[1];
    zi[i] = B->X[2];
    vi[i] = B->SRC;
  }
  nj = jbodies.size();
  for (B_iter B=jbodies.begin(); B!=jbodies.end(); B++) {
    int i = B-jbodies.begin();
    xj[i] = B->X[0];
    yj[i] = B->X[1];
    zj[i] = B->X[2];
    vj[i] = B->SRC;
  }
}

extern "C" void FMM_Laplace(int ni, double * xi, double * yi, double * zi, double * vi,
			    int nj, double * xj, double * yj, double * zj, double * vj) {
  args->numBodies = ni;
  logger->printTitle("FMM Parameters");
  args->print(logger->stringLength, P, baseMPI->mpirank);
  logger->printTitle("FMM Profiling");
  logger->startTimer("Total FMM");
  logger->startPAPI();
  const real_t cycle = 0.0;
  Bodies bodies(ni);
  for (B_iter B=bodies.begin(); B!=bodies.end(); B++) {
    int i = B-bodies.begin();
    B->X[0]   = xi[i];
    B->X[1]   = yi[i];
    B->X[2]   = zi[i];
    B->SRC    = 1;
    B->TRG[0] = vi[i];
    B->TRG[1] = 0;
    B->TRG[2] = 0;
    B->TRG[3] = 0;
    B->IBODY = i;
  }
  Bodies jbodies(nj);
  for (B_iter B=jbodies.begin(); B!=jbodies.end(); B++) {
    int i = B-jbodies.begin();
    B->X[0]   = xj[i];
    B->X[1]   = yj[i];
    B->X[2]   = zj[i];
    B->SRC    = vj[i];
    B->TRG    = 0;
    B->IBODY = i;
  }
  Cells cells = build->buildTree(bodies, localBounds);
  upDownPass->upwardPass(cells);
  Cells jcells = build->buildTree(jbodies, localBounds);
  upDownPass->upwardPass(jcells);
  treeMPI->setLET(jcells, localBounds, cycle);
  treeMPI->commBodies();
  treeMPI->commCells();
  traversal->dualTreeTraversal(cells, jcells, cycle, args->mutual);
  for (int irank=1; irank<baseMPI->mpisize; irank++) {
    treeMPI->getLET(jcells,(baseMPI->mpirank+irank)%baseMPI->mpisize);
    traversal->dualTreeTraversal(cells, jcells, cycle);
  }
  upDownPass->downwardPass(cells);
  logger->stopPAPI();
  logger->stopTimer("Total FMM");
  logger->printTitle("Total runtime");
  logger->printTime("Total FMM");
  for (B_iter B=bodies.begin(); B!=bodies.end(); B++) {
    int i = B->IBODY;
    vi[i] = B->TRG[0];
  }
}

void MPI_Shift(double * var, int &nold, int mpisize, int mpirank) {
  const int isend = (mpirank + 1          ) % mpisize;
  const int irecv = (mpirank - 1 + mpisize) % mpisize;
  int nnew;
  MPI_Request sreq, rreq;
  MPI_Isend(&nold, 1, MPI_DOUBLE, irecv, 0, MPI_COMM_WORLD, &sreq);
  MPI_Irecv(&nnew, 1, MPI_DOUBLE, isend, 0, MPI_COMM_WORLD, &rreq);
  MPI_Wait(&sreq, MPI_STATUS_IGNORE);
  MPI_Wait(&rreq, MPI_STATUS_IGNORE);
  double * buf = new double [nnew];
  MPI_Isend(var, nold, MPI_DOUBLE, irecv, 1, MPI_COMM_WORLD, &sreq);
  MPI_Irecv(buf, nnew, MPI_DOUBLE, isend, 1, MPI_COMM_WORLD, &rreq);
  MPI_Wait(&sreq, MPI_STATUS_IGNORE);
  MPI_Wait(&rreq, MPI_STATUS_IGNORE);
  for (int i=0; i<nnew; i++) {
    var[i] = buf[i];
  }
  nold = nnew;
  delete[] buf;
}

extern "C" void Direct_Laplace(int ni, double * xi, double * yi, double * zi, double * vi,
			       int nj, double * xj, double * yj, double * zj, double * vj) {
  const int Nmax = 1000000;
  double * x2 = new double [Nmax];
  double * y2 = new double [Nmax];
  double * z2 = new double [Nmax];
  double * v2 = new double [Nmax];
  for (int i=0; i<nj; i++) {
    x2[i] = xj[i];
    y2[i] = yj[i];
    z2[i] = zj[i];
    v2[i] = vj[i];
  }
  if (baseMPI->mpirank == 0) std::cout << "--- MPI direct sum ---------------" << std::endl;
  for (int irank=0; irank<baseMPI->mpisize; irank++) {
    if (baseMPI->mpirank == 0) std::cout << "Direct loop          : " << irank+1 << "/" << baseMPI->mpisize << std::endl;
    int n2 = nj;
    MPI_Shift(x2, nj, baseMPI->mpisize, baseMPI->mpirank);
    nj = n2;
    MPI_Shift(y2, nj, baseMPI->mpisize, baseMPI->mpirank);
    nj = n2;
    MPI_Shift(z2, nj, baseMPI->mpisize, baseMPI->mpirank);
    nj = n2;
    MPI_Shift(v2, nj, baseMPI->mpisize, baseMPI->mpirank);
    for (int i=0; i<ni; i++) {
      double pp = 0;
      for (int j=0; j<nj; j++) {
	double dx = xi[i] - x2[j];
	double dy = yi[i] - y2[j];
	double dz = zi[i] - z2[j];
	double R2 = dx * dx + dy * dy + dz * dz;
	double invR = 1 / std::sqrt(R2);
	if (R2 == 0) invR = 0;
	pp += v2[j] * invR;
      }
      vi[i] += pp;
    }
  }
  delete[] x2;
  delete[] y2;
  delete[] z2;
  delete[] v2;
}
