## Linux prototype for RDMA exokernel

#### Files
- `controller.c`: emulates the remote control plane of the exokernel. Current iteration is the RDMA sender
- `exokernel.c`: emulates the exokernel compute node of the exokernel architecture. Current iteration is the RDMA receiver, that sets up buffers to receive a binary to run locally
- `helloworld.c`: the binary that is shipped to the exokernel to run
- `elf.c`: a sample elf loader that loader a test binary (helloworld) into its memory space and jumps to the entry point

#### How to compile and run
To test RDMA functionality, you need to have two servers that are connected via some RDMA switch. The implementation currently uses RoCE protocol, so we don't rely on special Infiniband hardware.

On each server (one is the controller, one if the exokernel), run:
```
> make clean; make
```

The exokernel needs to start first:
```
> ./exokernel <controller_qp_num>
```

Then start the controller:
```
> ./controller <exokernel_qp_num>
```

For now, we hack the RDMA protocol and manually provide the qp num to each RDMA side. Otherwise, RDMA setup usually requires a handshake via TCP, where the two sides exchange information such as qp num to use for the other side. In this case, we do not want to go through the hassle of the TCP handshake, so we hardcode for now.

Usually this means that it takes 1-2 tries to get the qp numbers right. In particular, you can run `./exokernel` or `./controller` and inspect the output to get the current qp of that side. Then, kill execution and rerun while passing in the qp num on the command. QP numbers are always incremented by 1, so you need to increment whatever output you saw from the test run by 1.

#### TODO
- Instead of sending a binary from controller to exokernel, implement the RPCs that enables controller to remotely setup binary, like in elf.c, but using RDMA.
- This prototype serves as a reference implementation for the Bespin exokernel implementation. We should port this prototype to Bespin arch/unix/, and eventually to use native Bespin.
