#include "pluto.h"

extern void prizmo_set_radial_ncol_h2_c(double *);
extern void prizmo_set_radial_ncol_co_c(double *);
extern void prizmo_set_vertical_ncol_co_c(double *);
extern void prizmo_evolve_rho_c(double *, double *, double *, double *, double *, int *);
extern void prizmo_frac2n_c(double *, double *, double *);
extern void prizmo_rt_rho_c(double *, double *, double *, double *, double *);

/* ********************************************************************* */
void initialize_Microphysics(Grid *grid)
/*
 * Initialize the arrays for the column density and radiation flux
 * calculation, and find neighbor domains for parallel calculation.
 *
 * \param[in]     grid    pointer to array of Grid structures.
 *
 *********************************************************************** */
{
    int n, i, j, k;
    irradiation.neighbour.receive_rank = -1;
    irradiation.neighbour.send_rank = -1;

    irradiation.column_density = ARRAY_4D(3, NX3_TOT, NX2_TOT, NX1_TOT, double);
    irradiation.jflux = ARRAY_4D(NX3_TOT, NX2_TOT, NX1_TOT, NPHOTO, double);
    irradiation.jflux0 = ARRAY_1D(NPHOTO, double);
    irradiation.data_buffer = ARRAY_1D(NX2*NX3, double);
    irradiation.column_density_offset = ARRAY_1D(NX2*NX3, double);
    irradiation.tcpu = ARRAY_3D(NX3_TOT, NX2_TOT, NX1_TOT, double);

    for(n = 0; n < NX2 * NX3; ++n)
    {
        irradiation.column_density_offset[n] = 0.;
    }

    DOM_LOOP(k, j, i){
        irradiation.tcpu[k][j][i] = 0.;
    }

    find_CommunicationNeighbours(grid);
}

/*********************************************************************** */
void cleanup_Microphysics()
/*
 * Free arrays used for column density and radiation flux calculation.
 *
 *********************************************************************** */
{
    FreeArray4D((void *) irradiation.column_density);
    FreeArray4D((void *) irradiation.jflux);
    FreeArray1D((void *) irradiation.jflux0);
    FreeArray1D((void *) irradiation.data_buffer);
    FreeArray1D((void *) irradiation.column_density_offset);
    FreeArray3D((void *) irradiation.tcpu);
}

/* ********************************************************************* */
void Chemistry(Data_Arr v, double dt, Grid *grid)
/*
 * Calculate the chemical abundances in the computational domain.
 *
 * \param[in]     v       Data Array containing conservative variables
 * \param[in]     dt      time increment
 * \param[in]     grid    pointer to array of Grid structures. 
 *
 *********************************************************************** */
{
    int i, j, k, n;
    int verboseChem=0, prankPrint=-1, kPrint=-1, jPrint=-1, iPrint=-1;
    double abundance[NTRACER];
    double temperature_cgs, dt_cgs, density_cgs;
    clock_t t_chem_start, t_chem_end;

    DOM_LOOP(k, j, i){
        t_chem_start = clock();
        density_cgs = v[RHO][k][j][i]*UNIT_DENSITY;
        temperature_cgs = v[PRS][k][j][i]/v[RHO][k][j][i]*(KELVIN*g_inputParam[G_MU]);
        dt_cgs = dt*UNIT_LENGTH/UNIT_VELOCITY;

        NTRACER_LOOP(n) {
        	abundance[n-TRC] = v[n][k][j][i];
            //if (abundance[n-TRC] < 0.0) {
            //    printLog("! Chemistry: rho_tr%d = %8.2e", n-TRC, abundance[n-TRC]);
            //    Where (i, NULL);
            //}
	    }

	    if (prank == prankPrint && k == kPrint && j == jPrint && i==iPrint) verboseChem = 1;

    	if (verboseChem == 1)
	    {
	    	printLog("Calculating chemistry in cell %d %d %d\n", i, j, k);
	    	printLog("located at r=%e, theta=%f, phi=%f; with\n", grid->x[IDIR][i], grid->x[JDIR][j], grid->x[KDIR][k]);
	    	printLog("Density: %e\n", density_cgs);
	    	printLog("Temperature: %e\n", temperature_cgs);
	    	NTRACER_LOOP(n) printLog("tracer%d: %e\n", n-TRC, abundance[n-TRC]);
	    	LogFileFlush();
	    }
 
        // set incoming radial column density
        prizmo_set_radial_ncol_h2_c(&irradiation.column_density[1][k][j][i]);
	    if (verboseChem == 1)
	    {
	    	printLog("set radial_ncol_h2 to %e\n", &irradiation.column_density[1][k][j][i]);
	    	LogFileFlush();
	    }
        prizmo_set_radial_ncol_co_c(&irradiation.column_density[2][k][j][i]);
        // set excaping vertical column density
        prizmo_set_vertical_ncol_co_c(&irradiation.column_density[2][k][j][i]);
	    if (verboseChem == 1)
	    {
	    	printLog("set ncol_co to %e\n", &irradiation.column_density[2][k][j][i]);
	    	LogFileFlush();
	    }

        // update chemical abundances, temperature and radiation flux
        prizmo_evolve_rho_c(abundance, &density_cgs, &temperature_cgs, 
			irradiation.jflux[k][j][i], &dt_cgs, &verboseChem);

        v[PRS][k][j][i] = v[RHO][k][j][i]*temperature_cgs/(KELVIN*g_inputParam[G_MU]);

        NTRACER_LOOP(n) v[n][k][j][i] = abundance[n-TRC];

	    if (verboseChem == 1)
	    {
	    	printLog("Calculating chemistry complete\n");
	    	LogFileFlush();
	    }

        // Timing
        t_chem_end = clock();
        irradiation.tcpu[k][j][i] = ((double) (t_chem_end - t_chem_start)) / CLOCKS_PER_SEC;

    }
}

/*********************************************************************** */
void find_CommunicationNeighbour(int current_rank, 
		LocalDomainInfo *domain_info_array, int nproc, 
		CommunicationNeighbour* cn)
/*
 * Find all the neighbour computational domains the current rank has to send
 * and receive data
 *
 * \param[in]     current_rank       integer index of the current rank
 * \param[in]     domain_info_array  structure defining the boundaries of the current
 *                                   computational domain
 * \param[in]     nproc              total number of processors
 * \param[out]    cn                 structure containing the indices of processors
 *                                   the current rank has to communicate with
 *
 *********************************************************************** */
{
    int n = 0;
    LocalDomainInfo* cdi = &domain_info_array[current_rank];

    cn->receive_rank = -1;
    cn->send_rank = -1;

    for(n = 0; n < nproc; ++n)
    {
        if(n != current_rank)
        {
            if(cdi->x1_begin - 1 == domain_info_array[n].x1_end && cdi->x2_begin == domain_info_array[n].x2_begin && cdi->x2_end == domain_info_array[n].x2_end && cdi->x3_begin == domain_info_array[n].x3_begin && cdi->x3_end == domain_info_array[n].x3_end)
            {
                cn->receive_rank = n;
            }
            else if(cdi->x1_end + 1 == domain_info_array[n].x1_begin && cdi->x2_begin == domain_info_array[n].x2_begin && cdi->x2_end == domain_info_array[n].x2_end && cdi->x3_begin == domain_info_array[n].x3_begin && cdi->x3_end == domain_info_array[n].x3_end)
            {
                cn->send_rank = n;
            }
        }
    }
}

/************************************************************************ */
void find_CommunicationNeighbours(Grid *grid)
/*
 * Find the neighbour computational domain for the whole grid, distinguishing
 * between domains that are receiving and sending data to the other ones.
 *
 * \param[in]     grid    pointer to array of Grid structures.
 *
 ************************************************************************ */
{
    int nproc = 0, n = 0;
    LocalDomainInfo ldi;
    LocalDomainInfo *domain_info_array = NULL;
    LocalDomainInfo *item = NULL;
    CommunicationNeighbour* neighbour_info_array = NULL;
    MPI_Status status;

    MPI_Comm_size(MPI_COMM_WORLD, &nproc);

    ldi.x1_begin = grid->beg[IDIR];
    ldi.x1_end = grid->end[IDIR];

    ldi.x2_begin = grid->beg[JDIR];
    ldi.x2_end = grid->end[JDIR];

    ldi.x3_begin = grid->beg[KDIR];
    ldi.x3_end = grid->end[KDIR];

    if(prank == 0)
    {
        domain_info_array = malloc(sizeof(LocalDomainInfo) * nproc);
        neighbour_info_array = malloc(sizeof(CommunicationNeighbour) * nproc);
    }

    MPI_Gather(&ldi, sizeof(LocalDomainInfo), MPI_CHAR, domain_info_array, sizeof(LocalDomainInfo), MPI_CHAR, 0, MPI_COMM_WORLD);

    if(prank == 0)
    {
        for(n = 0; n < nproc; ++n)
        {
            find_CommunicationNeighbour(n,domain_info_array,nproc,&neighbour_info_array[n]);
        }

        for(n = 0; n < nproc; ++n)
        {
            if(n != 0)
            {
                MPI_Send(&neighbour_info_array[n],sizeof(CommunicationNeighbour),MPI_CHAR,n,0,MPI_COMM_WORLD);
            }
        }

        irradiation.neighbour.receive_rank = neighbour_info_array[0].receive_rank;
        irradiation.neighbour.send_rank = neighbour_info_array[0].send_rank;

        free(domain_info_array);
        free(neighbour_info_array);
    }
    else
    {
        MPI_Recv(&irradiation.neighbour, sizeof(CommunicationNeighbour), MPI_CHAR, 0, 0, MPI_COMM_WORLD, &status);
    }
}

/*********************************************************************** */
void calculate_Attenuation(Data_Arr v, Grid *grid)
/*
 * Calculate radiation attenuation in the whole domain calling PRIZMO.
 * 
 * WARNING: this calculation needs to be run in serial over the radial 
 * direction since each domain has to know the attenutation at its inner 
 * radial boundary before starting.
 *
 * TODO: restrict this calculation only for spherical coordinates.
 *
 * \param[in]     v       Data Array containing conservative variables
 * \param[in]     grid    pointer to array of Grid structures
 *
 *********************************************************************** */
{
    int k, j, i, l, n, rank;
    double density_cgs, temperature_cgs, dr_cgs;
    double abundance[NTRACER];
    double Lflux[NPHOTO];
    double jflux[NPHOTO];
    MPI_Status status;

    for (rank=0; rank<g_nprocs; rank++){
        if(prank == rank){
            KDOM_LOOP(k){
                JDOM_LOOP(j){
                    if(irradiation.neighbour.receive_rank == -1 && irradiation.neighbour.send_rank != -1){ //no neighbor in between the star and the current domain
                        //initialize radiation luminosity
                        NPHOTO_LOOP(n) {
                            Lflux[n] = irradiation.jflux0[n];
                        }
	                }
	                else {
                        MPI_Recv(Lflux, NPHOTO, MPI_DOUBLE, irradiation.neighbour.receive_rank, 0, MPI_COMM_WORLD, &status);
	                }
                    //scale to fluxes
                    NPHOTO_LOOP(n) {
                        jflux[n] = Lflux[n]/(4.0*CONST_PI*grid->x[IDIR][IBEG]*grid->x[IDIR][IBEG]);
                        irradiation.jflux[k][j][IBEG][n] = jflux[n];
                    }

                    IDOM_LOOP(i){
                        density_cgs = v[RHO][k][j][i] * UNIT_DENSITY;
                        dr_cgs = grid->dx[IDIR][i]*UNIT_LENGTH;
                        temperature_cgs = v[PRS][k][j][i]/v[RHO][k][j][i]*(KELVIN*g_inputParam[G_MU]);
		
		                NTRACER_LOOP(l) abundance[l-TRC] = v[l][k][j][i];
		
		                //calculate radiation attenuation at the radial cell i
                        prizmo_rt_rho_c(abundance, &density_cgs, &temperature_cgs, jflux, &dr_cgs);
		
		                //assign attenuated and diluted radiation flux to the next radial cell
                        NPHOTO_LOOP(n) {
                            jflux[n] *= (grid->x[IDIR][i]*grid->x[IDIR][i])/(grid->x[IDIR][i+1]*grid->x[IDIR][i+1]);
                            irradiation.jflux[k][j][i+1][n] = jflux[n];
                        }
                    }
	                if(irradiation.neighbour.send_rank != -1) {
                        //scale to luminosity
                        NPHOTO_LOOP(n) {
                            Lflux[n] = jflux[n]*(4.0*CONST_PI*grid->x[IDIR][IEND]*grid->x[IDIR][IEND]);
                        }
                        MPI_Send(Lflux, NPHOTO, MPI_DOUBLE, irradiation.neighbour.send_rank, 0, MPI_COMM_WORLD);
                    }
                }
	        }
        }
        MPI_Barrier(MPI_COMM_WORLD);
    }
}

/*********************************************************************** */
void calculate_ColumnDensity_perDomain(Data_Arr v, Grid *grid, int val)
/*
 * Calculate the total column density or for a specific species in the
 * local computational domain, and send the 
 *
 * \param[in]     v       Data Array containing conservative variables
 * \param[in]     grid    pointer to array of Grid structures
 * \param[in]     val     integer define which column density we are calculating
 *                        (0 = total, 1= H2, 2=CO)
 *
 *********************************************************************** */
{
    int k, j, i, l;
    double column_density;
    double density_cgs, dr_cgs;
    double mpart = g_inputParam[GAMMA_EOS]*CONST_amu;
    double abundance[NTRACER], number_density[NTRACER];
    MPI_Status status;

    NTRACER_LOOP(l){
        abundance[l-TRC] = 0.;
        number_density[l-TRC] = 0.;
    }

    // Calculate column density in the current computational domain
    KDOM_LOOP(k){
        JDOM_LOOP(j){
            //initialize column density for each radial sweep
            column_density = 0.0;
            irradiation.column_density[val][k][j][IBEG] = column_density;

            IDOM_LOOP(i){
                density_cgs = v[RHO][k][j][i] * UNIT_DENSITY;
		        dr_cgs = grid->dx[IDIR][i]*UNIT_LENGTH;

                NTRACER_LOOP(l) abundance[l-TRC] = v[l][k][j][i];
                prizmo_frac2n_c(abundance, &density_cgs, number_density);
                
		        if (val == 0) {
                    column_density += density_cgs/mpart*dr_cgs;
                } else if (val == 1) {
                    column_density += number_density[IDX_CHEM_H2-TRC] * dr_cgs;
                } else {
                    column_density += number_density[IDX_CHEM_CO-TRC] * dr_cgs;
                }
                irradiation.column_density[val][k][j][i+1] = column_density;
            }
        }
    }

    // Calculate the column density offset for each processor in density_offset
    if(irradiation.neighbour.receive_rank != irradiation.neighbour.send_rank)//check if more than one process is in the communicator (nproc > 1)
    {
        if(irradiation.neighbour.receive_rank == -1 && irradiation.neighbour.send_rank != -1) //no neighbor in between the star and the current domain
        {
            l = 0;
            i = IEND+1;
            KDOM_LOOP(k){
                JDOM_LOOP(j){
                    irradiation.data_buffer[l] = irradiation.column_density[val][k][j][i];
                    irradiation.column_density_offset[l] = 0.;
                    l++;
                }
            }
            MPI_Send(irradiation.data_buffer, NX2*NX3, MPI_DOUBLE, irradiation.neighbour.send_rank, 0, MPI_COMM_WORLD);
        }
        else
        {
            MPI_Recv(irradiation.column_density_offset, NX2*NX3, MPI_DOUBLE, irradiation.neighbour.receive_rank, 0, MPI_COMM_WORLD, &status);

            l = 0;
            i = IEND+1;
            KDOM_LOOP(k){
                JDOM_LOOP(j){
                    irradiation.data_buffer[l] = irradiation.column_density_offset[l] + irradiation.column_density[val][k][j][i];
                    l++;
                }
            }

            if(irradiation.neighbour.send_rank != -1)
            {
                MPI_Send(irradiation.data_buffer, NX2*NX3, MPI_DOUBLE, irradiation.neighbour.send_rank, 0, MPI_COMM_WORLD);
            }

        }
        MPI_Barrier(MPI_COMM_WORLD);
    }
}

/*********************************************************************** */
void calculate_ColumnDensity(Data_Arr v, Grid *grid)
/*
 * Calculate the column density in the whole computational domain.
 *
 * \param[in]     v       Data Array containing conservative variables
 * \param[in]     grid    pointer to array of Grid structures
 *
 *********************************************************************** */
{
    int k, j, i, l, n;

    for (l=0; l<3; l++)
    {
	// Calculate column densities in each computational domain
        calculate_ColumnDensity_perDomain(v, grid, l);

	// Add the column density offsets from previous radial domains
        n = 0;
        KDOM_LOOP(k){
            JDOM_LOOP(j){
                IDOM_LOOP(i){
                    irradiation.column_density[l][k][j][i] += irradiation.column_density_offset[n];
                }
                n++;
            }
        }
    }
}

