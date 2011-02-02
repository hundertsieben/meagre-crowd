/* Meagre-Crowd: A sparse distributed matrix solver testbench for performance benchmarking.
 * Copyright (C) 2010 Alistair Boyle <alistair.js.boyle@gmail.com>
 *
 *     This file is part of Meagre-Crowd.
 *
 *     Meagre-Crowd program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU Lesser General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU Lesser General Public License for more details.
 *
 *     You should have received a copy of the GNU Lesser General Public License
 *     along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
/* Example program using the C interface to the
* double precision version of MUMPS, dmumps_c.
* We solve the system A x = RHS with
* A = diag(1 2) and RHS = [1 4]ˆT
* Solution is [1 2]ˆT */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <libgen.h>
#include <string.h>
#include <float.h> // machine epsilon: LDBL_EPSILON, DBL_EPSILON, FLT_EPSILON

#include <mpi.h>

#include "args.h"
#include "perftimer.h"
#include "file.h"
#include "matrix.h"
#include "solvers.h"

// test result of matrix computations
// returns: 1=match w/in precision, 0=non-matching
// TODO refactor mv to matrix.h, operate on matrix_t objects
// TODO cmp_matrix()
int results_match( matrix_t* expected_matrix, matrix_t* result_matrix, const double precision );
int results_match( matrix_t* expected_matrix, matrix_t* result_matrix, const double precision ) {
  assert( expected_matrix != NULL );
  assert( result_matrix != NULL );
  assert( expected_matrix->format != INVALID );
  assert( result_matrix->format != INVALID );
  assert( expected_matrix->data_type == REAL_DOUBLE );
  assert( result_matrix->data_type == REAL_DOUBLE );
  assert( result_matrix->m == expected_matrix->m );
  assert( result_matrix->n == expected_matrix->n );
  assert( result_matrix->n == 1 );

  int ret;
  ret = convert_matrix( expected_matrix, DROW, FIRST_INDEX_ZERO );
  assert( ret == 0 );
  ret = convert_matrix( result_matrix,   DROW, FIRST_INDEX_ZERO );
  assert( ret == 0 );

  const double* expected = expected_matrix->dd;
  const double* result = result_matrix->dd;
  const unsigned int n = result_matrix->m;
  int i;
  for ( i = 0;i < n;i++ ) {
    if (( result[i] < ( expected[i] - precision ) ) ||
        ( result[i] > ( expected[i] + precision ) ) ) {
      return 0;
    }
  }
  return 1;
}


int main( int argc, char ** argv ) {
  int retval;
  const int false = 0;
  const int extra_timing = 0; // allow extra timing: initialization, matrix load, etc.

  // handle command-line arguments
  struct parse_args* args = calloc( 1, sizeof( struct parse_args ) );
  // TODO should default to appropriate epsilon for solver, may need to be *2 or some larger value given numerical instability? should print out epsilon of solution in verbose mode
  args->expected_precision = 5e-15; // TODO was DBL_EPSILON=1.11e-16 but not stored with enough digits? // default to machine epsilon for 'double'
  assert( args != NULL ); // calloc failure
  if (( retval = parse_args( argc, argv, args ) ) != EXIT_SUCCESS ) {
    free( args );
    return retval;
  }


  // initialize MPI
  perftimer_t* timer = perftimer_malloc();
  if ( extra_timing && args->rep == 0 ) {
    perftimer_inc( timer, "initialization", -1 );
    perftimer_adjust_depth( timer, + 1 );
    perftimer_inc( timer, "MPI init", -1 );
  }

  // if this solver is not thread/mpi safe, report an error if its been launched that way
  const int uses_mpi     = solver_uses_mpi( args->solver );
  const int requires_mpi = solver_requires_mpi( args->solver );
  const int uses_omp     = solver_uses_omp( args->solver );
  const int requires_omp = solver_requires_omp( args->solver );
  int c_mpi;
  {
    const char* mpi_world_size = getenv( "OMPI_COMM_WORLD_SIZE" );
    if ( mpi_world_size == NULL ) { // no env value configured
      c_mpi = 0;
      // printf( "no OMPI_COMM_WORLD_SIZE\n" );
    }
    else { // works #ifdef OPEN_MPI -- we're using openMPI rather than lam or mpich...
      int ret = sscanf( mpi_world_size, "%d", &c_mpi );
      assert( ret == 1 );
      // printf( "OMPI_COMM_WORLD_SIZE=%d\n", c_mpi );
    }
  }
  int c_omp = 0; // no sane checks for openMP yet...
  const int is_mpi = ( requires_mpi || ( uses_mpi && ( c_mpi != 0 ) ) );
  const int is_omp = ( requires_omp || ( uses_omp && ( c_omp != 0 ) ) );
  const int is_single_threaded = ( !is_mpi && !is_omp );


  // start up MPI, if we're using it
  const int is_single_threaded_but_mpi = is_single_threaded && ( c_mpi > 1 );
  if ( is_mpi || is_single_threaded_but_mpi ) {
    int ierr;
    ierr = MPI_Init( &argc, &argv );
    assert( ierr == 0 );
    ierr = MPI_Comm_rank( MPI_COMM_WORLD, &( args->mpi_rank ) );
    assert( ierr == 0 );
    ierr = MPI_Comm_size( MPI_COMM_WORLD, &c_mpi );
    assert( ierr == 0 );
  }

  // wait till after firing up MPI, so we only put out an error on the rank=0 machine
  if ( is_single_threaded_but_mpi ) {
    if ( args->mpi_rank == 0 )
      fprintf( stderr, "error: selected solver (%s) is single threaded but was launched with %d threads\n",
               solver2str( args->solver ), c_mpi );
    int ierr = MPI_Finalize();
    assert( ierr == 0 );
    return 10;
  }

  // Define the problem on the host
  matrix_t* b = malloc_matrix();
  matrix_t* expected = malloc_matrix();
  matrix_t* A = malloc_matrix();
  matrix_t* rhs = malloc_matrix();
  unsigned int m = 0; // rows
  if ( args->mpi_rank == 0 ) {

    if ( extra_timing && args->rep == 0 ) {
      perftimer_adjust_depth( timer, -1 );
      perftimer_inc( timer, "input", -1 );
      perftimer_adjust_depth( timer, + 1 );
    }

    if ( extra_timing && args->rep == 0 )
      perftimer_inc( timer, "load", -1 );

    if (( retval = load_matrix( args->input, A ) ) != 0 ) {
      return retval;
    }
    assert(validate_matrix(A) == 0);
    m = matrix_rows( A );
    // TODO load a rhs from the --input matrix file

    // allocate an sequentially numbered right-hand side of A.m rows
    // TODO warn if there is already a rhs loaded
    if ( args->rhs != NULL ) {
      if (( retval = load_matrix( args->rhs, b ) ) != 0 ) {
        return retval;
      }
      assert( b->m == m ); // rows must match // TODO nice error (user could load some random matrix file, also testcases)
      assert( b->n == 1 ); // TODO can only handle single column vector currently

      // convert from COO vector to dense format vector
      // TODO this is redundant, since it needs to be done in the solver ... do it there, remove this later
      convert_matrix( b, DCOL, FIRST_INDEX_ZERO );
      // TODO but don't remove this here, since we want to test dense-matrix performance? OR do we need a new option! OR do we decide based on matrix sparsity? (EIT is sparse...)
    }
    else {
      assert( b != NULL ); // malloc failure
      *b = ( matrix_t ) {0};
      b->m = m;
      b->n = 1;
      b->nz = m;
      b->format = DCOL;
      b->dd = malloc( m * sizeof( double ) );
      b->data_type = REAL_DOUBLE;
      { // initialize right-hand-side (b)
        int i;
        double* d = b->dd;
        for ( i = 0;i < m;i++ ) {
          *d = i;
          d++;
        }
      }
    }
    assert(validate_matrix(b) == 0);

    if ( args->expected != NULL ) {
      // TODO refactor: this is a cut and paste of the loader for 'b'
      if (( retval = load_matrix( args->expected, expected ) ) != 0 ) {
        return retval;
      }
      assert( expected->m == m ); // rows must match // TODO nice error (user could load some random matrix file, also testcases)
      assert( expected->n == 1 ); // TODO can only handle single column vector currently

      // convert from COO vector to dense format vector
      // TODO this is redundant, since it needs to be done in the solver ... do it there, remove this later
      convert_matrix( b, DCOL, FIRST_INDEX_ZERO );
      assert(validate_matrix(expected) == 0);
    }


    if ( extra_timing && args->rep == 0 ) {
      perftimer_inc( timer, "solver", -1 );
      perftimer_inc( timer, "rhs", -1 );
    }

    // verbose output
    if ( args->verbosity >= 1 ) {
      assert( A != NULL );
      int ierr = convert_matrix( A, SM_COO, FIRST_INDEX_ZERO );
      assert( ierr == 0 );
      const char* sym, *location, *type;
      switch ( A->sym ) {
        case SM_UNSYMMETRIC:
          sym = "unsymmetric";
          break;
        case SM_SYMMETRIC:
          sym = "symmetric";
          break;
        case SM_SKEW_SYMMETRIC:
          sym = "skew symmetric";
          break;
        case SM_HERMITIAN:
          sym = "hermitian";
          break;
        default:
          assert( false ); // fell through
      }
      if (( args->verbosity < 2 ) || ( A->sym == SM_UNSYMMETRIC ) ) {
        location = "";
      }
      else {
        switch ( A->location ) {
          case UPPER_TRIANGULAR:
            location = " (upper)";
            break;
          case LOWER_TRIANGULAR:
            location = " (lower)";
            break;
          case BOTH:
            location = "";
            break; // nothing
          default:
            assert( false );
        }
      }
      switch ( A->data_type ) {
        case REAL_DOUBLE:
          type = "real";
          break;
        case REAL_SINGLE:
          type = "real (single-precision)";
          break;
        case COMPLEX_DOUBLE:
          type = "complex";
          break;
        case COMPLEX_SINGLE:
          type = "complex (single-precision)";
          break;
        case SM_PATTERN:
          type = "pattern";
          break;
        default:
          assert( false ); // fell through
      }
      printf( "Ax=b: A is %zux%zu, nz=%zu, %s%s, %s, b is %zux%zu, nz=%zu\nsolved with %s on %d core%s, %d thread%s\n",
              A->m, A->n, A->nz,
              sym, location, type,
              b->m, b->n, b->nz,
              solver2str( args->solver ),
              c_mpi, c_mpi == 1 ? "" : "s", c_omp, c_omp == 1 ? "" : "s" );

      if ( args->verbosity >= 2 ) {
        printf_matrix( "  A", A );
      }

      if ( args->verbosity >= 2 ) { // show the rhs matrix
        printf_matrix( "  b", b );
      }
    }

    if ( extra_timing && args->rep == 0 )
      perftimer_adjust_depth( timer, -1 );
    //destroy_sparse_matrix (A); // TODO can't release it unless we're copying it...
  }

  solver_state_t* state = solver_init( args->solver, args->verbosity, args->mpi_rank, timer );

  int r = 0;
  do {
    if ( r != 0 )
      perftimer_restart( &timer );

    solver_solve( state, A, b, rhs ); // TODO rhs -> x

    r++;
  }
  while ( r < args->rep );

  if ( extra_timing && args->rep == 0 ) {
    perftimer_inc( timer, "clean up", -1 );
    perftimer_adjust_depth( timer, -1 );
    perftimer_inc( timer, "output", -1 );
  }

  // print/store result
  if (( args->mpi_rank == 0 ) && ( args->output != NULL ) ) {
    if ( strncmp( args->output, "-", 2 ) == 0 ) {
      printf_matrix( "  x", rhs );
    }
    else {
      // TODO test args->output to decide if its an acceptable filename before now
      // TODO really, we shouldn't care what the file name is, just the file format...
      if ( save_matrix( rhs, args->output ) != 0 )
        retval = 12; // TODO normalize error codes so they are defined in one place (defines?)
    }
  }

  // test result?
  if (( args->mpi_rank == 0 ) && ( expected->format != INVALID ) ) {
    perftimer_inc( timer, "test", -1 );
    if ( results_match( expected, rhs, args->expected_precision ) ) {
      retval = 0;
      printf( "PASS\n" );
    }
    else {
      retval = 100;
      printf( "FAIL\n" );
    }
  }

  solver_finalize( state );

  // clean up matrices
  free_matrix( b );
  free_matrix( expected );
  free_matrix( A );
  free_matrix( rhs );

  // close down MPI
  if ( extra_timing && args->rep == 0 ) {
    perftimer_inc( timer, "clean up", -1 );
    perftimer_adjust_depth( timer, + 1 );

    perftimer_inc( timer, "MPI", -1 );
  }

  if ( is_mpi ) {
    int ierr = MPI_Finalize();
    assert( ierr == 0 );
  }

  // show timing info, if requested, to depth N
  if ( extra_timing && args->rep == 0 ) {
    perftimer_adjust_depth( timer, -1 );
    perftimer_inc( timer, "finished", -1 );
  }
  if ( args->mpi_rank == 0 ) {
    if ( args->timing_enabled == 1 ) {
      perftimer_printf_csv_header( timer, 2 );
      perftimer_printf_csv_body( timer, 2 );
    }
    else if ( args->timing_enabled != 0 ) {
      perftimer_printf( timer, args->timing_enabled - 2 );
    }
  }
  perftimer_free( timer );
  free( args );
  return retval;
}
