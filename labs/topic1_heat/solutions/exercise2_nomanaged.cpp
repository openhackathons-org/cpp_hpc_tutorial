/*
 * SPDX-FileCopyrightText: Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

//! Solves heat equation in 2D, see the README.

#include <chrono>
#include <memory>
#include <iostream>
#include <cassert>
#include <fstream>
#include <mpi.h>
// DONE: added parallel algorithm and ranges includes
#include <algorithm>
#include <execution>
#include <utility> // for std::pair
#include <numeric> // for std::transform_reduce
#if defined(__clang__)
  // clang does not support libstdc++ ranges
  #include <range/v3/all.hpp>
  namespace views = ranges::views;
#elif __cplusplus >= 202002L
  #include <ranges>
  namespace views = std::views;
  namespace ranges = std::ranges;
#endif
#if defined(__NVCOMPILER)
  #include <thrust/device_vector.h>
  #include <thrust/iterator/counting_iterator.h>
#endif

// Problem parameters
struct parameters {
  double dx, dt;
  long nx, ny, ni;
  int rank = 0, nranks = 1;

  static constexpr double alpha() { return 1.0; }  // Thermal diffusivity

  parameters(int argc, char *argv[]) {
    if (argc != 4) {
      std::cerr << "ERROR: incorrect arguments" << std::endl;
      std::cerr << "  " << argv[0] << " <nx> <ny> <ni>" << std::endl;
      std::terminate();
    }
    nx = std::stoll(argv[1]);
    ny = std::stoll(argv[2]);
    ni = std::stoll(argv[3]);
    dx = 1.0 / nx;
    dt = dx * dx / (5. * alpha());
  }

  long nit() { return ni; }
  long nout() { return 1000; }
  long nx_global() { return nx * nranks; }
  long ny_global() { return ny; }
  double gamma() { return alpha() * dt / (dx * dx); }
};

// Index into the memory using row-major order:
long index(long x, long y, parameters p) {
    assert(x >= 0 && x < p.nx);
    assert(y >= 0 && y < p.ny);
    return x * p.ny + y;
};

// Finite-difference stencil
double stencil(double *u_new, double *u_old, long x, long y, parameters p) {
  auto idx = [=](auto x, auto y) { return index(x, y, p); };
  // Apply boundary conditions:
  if (y == 1) {
    u_old[idx(x, y - 1)] = 0;
  }
  if (y == (p.ny - 2)) {
    u_old[idx(x, y + 1)] = 0;
  }
  // These boundary conditions are only impossed by the ranks at the end of the domain:
  if (p.rank == 0 && x == 1) {
    u_old[idx(x - 1, y)] = 1;
  }
  if (p.rank == (p.nranks - 1) && x == p.nx) {
    u_old[idx(x + 1, y)] = 0;
  }

  u_new[idx(x, y)] = (1. - 4. * p.gamma()) * u_old[idx(x, y)] + p.gamma() * (u_old[idx(x + 1, y)] + u_old[idx(x - 1, y)] + u_old[idx(x, y + 1)] +
                                u_old[idx(x, y - 1)]);

  return 0.5 * u_new[idx(x, y)] * u_new[idx(x, y)] * p.dx * p.dx;
}

// 2D grid of indicies
struct grid {
  long x_start, x_end, y_start, y_end;
};

double stencil(double *u_new, double *u_old, grid g, parameters p) {
  // Map the 2D strided iteration space (x0, xN) * (y0, yN) to 1D
  long dx = g.x_end - g.x_start;
  long dy = g.y_end - g.y_start;
  long n = dx * dy; // #of elements
#if defined(__NVCOMPILER)
  // With NVHPC we can use Thrust's counting iterator
  auto b = thrust::counting_iterator<long>(0);
  auto e = thrust::counting_iterator<long>(n);
#else
  auto ints = views::iota((long)0, n);
  auto b = ints.begin();
  auto e = ints.end();
#endif

  // Recover the 2D strided iteration space from the 1D iteration space
  auto split = [x_start = g.x_start, y_start = g.y_start, dy](long i) -> std::pair<long, long> {
    return {i / dy + x_start, i % dy + y_start};
  };

  return std::transform_reduce(std::execution::par, b, e, 0.,
                               // Binary reduction
                               std::plus<>{},
                               // Unary Transform
                               [=, u_old = u_old, u_new = u_new](long i) {
                                 auto [x, y] = split(i);
                                 return stencil(u_new, u_old, x, y, p);
                               });
}

double internal(double* u_new, double* u_old, parameters p) {
    grid g { .x_start = 2, .x_end = p.nx, .y_start = 1, .y_end = p.ny - 1 };
    return stencil(u_new, u_old, g, p);
}

double prev_boundary(double* u_new, double* u_old, parameters p) {
    // Send window cells, receive halo cells
    if (p.rank > 0) {
      // Send bottom boundary to bottom rank
      MPI_Send(u_old + p.ny, p.ny, MPI_DOUBLE, p.rank - 1, 0, MPI_COMM_WORLD);
      // Receive top boundary from bottom rank
      MPI_Recv(u_old + 0, p.ny,  MPI_DOUBLE, p.rank - 1, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
    grid g { .x_start = p.nx, .x_end = p.nx + 1, .y_start = 1, .y_end = p.ny - 1 };
    return stencil(u_new, u_old, g, p);
}

double next_boundary(double* u_new, double* u_old, parameters p) {
    if (p.rank < p.nranks - 1) {
        // Receive bottom boundary from top rank
        MPI_Recv(u_old + (p.nx + 1) * p.ny, p.ny, MPI_DOUBLE, p.rank + 1, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        // Send top boundary to top rank, and
        MPI_Send(u_old + p.nx * p.ny, p.ny, MPI_DOUBLE, p.rank + 1, 1, MPI_COMM_WORLD);
    }
    grid g { .x_start = 1, .x_end = 2, .y_start = 1, .y_end = p.ny - 1 };
    return stencil(u_new, u_old, g, p);
}

void initialize(double *u_new, double *u_old, long n) {
   // DONE: initialization using parallel algorithms
   std::fill_n(std::execution::par_unseq, u_new, n, 0.);
   std::fill_n(std::execution::par_unseq, u_new, n, 0.);
}

int main(int argc, char *argv[]) {
  // Parse CLI parameters
  parameters p(argc, argv);
    
  // Initialize MPI with multi-threading support
  int mt;
  MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &mt);
  if (mt != MPI_THREAD_MULTIPLE) {
    std::cerr << "MPI cannot be called from multiple host threads" << std::endl;
    std::terminate();
  }
  MPI_Comm_size(MPI_COMM_WORLD, &p.nranks);
  MPI_Comm_rank(MPI_COMM_WORLD, &p.rank);

  // Allocate memory
  long n = (p.nx + 2) * p.ny;  // Needs to allocate 2 halo layers
#if defined(_NVHPC_STDPAR_GPU)
  auto u_new_ = thrust::device_vector<double>(n);
  auto u_old_ = thrust::device_vector<double>(n);
  auto u_new = u_new_.data().get();
  auto u_old = u_old_.data().get();
#else
  auto u_new_ = std::vector<double>(n);
  auto u_old_ = std::vector<double>(n);
  auto u_new = u_new_.data();
  auto u_old = u_old_.data();
#endif
    
  // Initial condition
  initialize(u_new, u_old, n);
    
  // Time loop
  using clk_t = std::chrono::steady_clock;
  auto start = clk_t::now();

  for (long it = 0; it < p.nit(); ++it) {
    double energy = 0.;
    // Exchange and compute domain boundaries:
    energy += prev_boundary(u_new, u_old, p);
    energy += next_boundary(u_new, u_old, p);
    energy += internal(u_new, u_old, p);
      
    // Reduce the energy across all neighbors to the rank == 0, and print it if necessary:
    MPI_Reduce(p.rank == 0 ? MPI_IN_PLACE : &energy, &energy, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    if (p.rank == 0 && it % p.nout() == 0) {
      std::cerr << "E(t=" << it * p.dt << ") = " << energy << std::endl;
    }
    std::swap(u_new, u_old);
  }

  auto time = std::chrono::duration<double>(clk_t::now() - start).count();
  auto grid_size = static_cast<double>(p.nx * p.ny * sizeof(double) * 2) / 1e9; // GB
  auto memory_bw = grid_size * static_cast<double>(p.nit()) / time; // GB/s
  if (p.rank == 0) {
    std::cerr << "Domain " << p.nx << "x" << p.ny << " (" << grid_size << " GB): " << memory_bw << " GB/s" << std::endl;
  }
  
  // Write output to file
  MPI_File f;
  MPI_File_open(MPI_COMM_WORLD, "output", MPI_MODE_CREATE | MPI_MODE_WRONLY, MPI_INFO_NULL, &f);
  auto header_bytes = 2 * sizeof(long) + sizeof(double);
  auto values_per_rank = p.nx * p.ny;
  auto values_bytes_per_rank = values_per_rank * sizeof(double);
  MPI_File_set_size(f, header_bytes + values_bytes_per_rank * p.nranks);
  MPI_Request req[3] = {MPI_REQUEST_NULL, MPI_REQUEST_NULL, MPI_REQUEST_NULL};
  if (p.rank == 0) {
    long total[2] = {p.nx * p.nranks, p.ny};
    double time = p.nit() * p.dt;
    MPI_File_iwrite_at(f, 0, total, 2, MPI_UINT64_T, &req[1]);
    MPI_File_iwrite_at(f, 2 * sizeof(long), &time, 1, MPI_DOUBLE, &req[2]);
  }
  auto values_offset = header_bytes + p.rank * values_bytes_per_rank;
  MPI_File_iwrite_at(f, values_offset, u_new + p.ny, values_per_rank, MPI_DOUBLE, &req[0]);
  MPI_Waitall(p.rank == 0 ? 3 : 1, req, MPI_STATUSES_IGNORE);
  MPI_File_close(&f);
    
  MPI_Finalize();

  return 0;
}
