## Running the Project in Vitis HLS (UI Workflow)

---

### 1) Open the Project

Launch **Vitis HLS**.

From the welcome screen, set the workspace:  
This is just the repo **Non-Pipelined_RISCV_CPU**

---

### 2) Verify Source Files

In the **Sources** view, confirm these are added:

- `src/core.cpp`
  
In **Test Bench**:

- Your testbench (e.g., `Testbench_elf_batch.cpp`, `Testbench_elf`, or `Testbench_HardCoded` ) (Testbench_elf is the only one that should be used at the moment)

If anything is missing, **Add Sources** (right-click **Sources → Add Files…**).

---

### 3) C Simulation Settings

**Program Arguments (important):** add your test path(s).  
(This should be saved in `hls_config.cfg` but sometimes the path doesn't like to show up automatically.  
You can copy it from the config underneath the **C Simulation** section if it doesn't show up after a couple times of clicking run.)
(Here it is as well for the batch) `../../../../Benchmarks/rv32ui-p-benchmarks`

The Path is different depending on the testbench. If you are using the batch version then it simply needs to be passed the file like in the example above. If you are looking at an indivdual test using the other testbench
then it needs to be in the path as well (ex: `../../../../Benchmarks/rv32ui-p-benchmarks/rsort.riscv`). There is no argument for the hard coded test bench. This means to run a different test you need to type in the path
inside of the testbench. It needs to be an absolute path that points to the benchmark similar to the normal version. This is used in cosim to avoid any issues with relative paths as cosim will run in a differnt directory.

Make sure that in the config file under General/C Synthesis sources the CFLAGS, CSIMFLAGS have the argument: -I ./include
Aso put this under the C testbench CFLAGS section (These should be here from the config but just in case they aren't)

---

### 4) Run C Simulation

In the **Flow** or **Flow Navigator** panel, click **C Simulation ▶ Run**.

If your arguments are correct, the tests will execute and print **SUMMARY: _ Passed, _ Failed** lines from the testbench.
All tests should be passing that are included here. It is importatnt that each .riscv file is included as a testbench file. This is different from the non-pipelined version,
but it makes it easier with the paths.

The regular and hardcoded testbenchs are similar and will also print PASS or FAIL with the error code.

---

### 5) Run C Synthesis

No matter what testbench is currently used C synthesis will run.

In the **Flow** or **Flow Navigator** panel, click **C Synthesis ▶ Run**.

No arguments need to be changed here. The default of 5ns and the exact FPGA are fine.
This will generate the actual VHDL and Verilog files. These can be found in the path `Non-Pipelined_RISCV_CPU\Global_Core_Revised\Global_Core_Revised\hls\syn`. Here there are 3 folders. 1 with the VHDL, 1 With the Verilog, and 1 containing all of the logs.

---


### 6) Run C/RTL COSIM

This is **only** ment to be run with `Testbench_elf` Test Bench. This is to do with the need for an abosolute path as the files are located in a different spot than the argument used previously.
It is easy to change the abosolute path in the testbench to test other benchmarks. Just like in the C-sim, all of the Benchmarks need to be included as testbench files.

In the **Flow** or **Flow Navigator** panel, click **C/RTL COSIMULATION ▶ Run**.

This will take a few minutes to run. It will also use about 20GB of space for files generated during the test. Previously when using 1 function call in the testbench, this step was generating hundreds more GB of test files.
With the current batch runner it is significantly faster and much more space efficient.

When it Finishs you will see a pass or fail at the end. This represents if it matches the C Sim data tested. So far the Qsort and Rsort have been tested and pass.
This is a good sign as all of the other tests pass the CSIM and these tests are alreay passing COSIM.

---


## 7) Results
### core.cpp — Pipelined Implementation, Inline Off, Simple Cache Line + D-RAM Cache (Utilizes FPGA DDR memory)

| Benchmark | Start Clock | End Clock | Total Cycles | # Instructions | Stall  | RTL CPI    | CLK      |
|-----------|-------------|-----------|--------------|-----------------|--------|------------|----------|
| QSORT     | 39          | 5013509   | 5013470      | 233942          | 102013 | 21.43039728 | 273.97MHz |
| TOWERS    | 39          | 290145    | 290106       | 11938           | 2676   | 24.30105545 | 273.97MHz |
| MEMCPY    | 39          | 746602    | 746563       | 37074           | 4629   | 20.13710417 | 273.97MHz |
| MULTIPLY  | 39          | 926685    | 926646       | 60594           | 8584   | 15.29270225 | 273.97MHz |
| RSORT     | 39          | 8441249   | 8441210      | 372918          | 21706  | 22.635566   | 273.97MHz |
| DHRYSTONE | 39          | 5167805   | 5167766      | 231368          | 45786  | 22.33569897 | 273.97MHz |
| VVADD     | 39          | 256473    | 256434       | 11072           | 2048   | 23.16058526 | 273.97MHz |
| MEDIAN    | 39          | 359330    | 359291       | 15427           | 4095   | 23.28975173 | 273.97MHz |

### onchip_core.cpp — Pipelined Implementation, Onchip Memory Only

| Benchmark | Start Clock | End Clock | Total Cycles | # Instructions | Stall  | RTL CPI    | CLK      |
|-----------|-------------|-----------|--------------|-----------------|--------|------------|----------|
| QSORT     | 39          | 1679854   | 1679815      | 233942          | 102013 | 7.180476357 | 303.34MHz |
| TOWERS    | 39          | 73149     | 73110        | 11938           | 2676   | 6.124141397 | 303.34MHz |
| MEMCPY    | 39          | 208594    | 208555       | 37074           | 4629   | 5.62537088  | 303.34MHz |
| MULTIPLY  | 39          | 345969    | 345930       | 60594           | 8584   | 5.708981087 | 303.34MHz |
| RSORT     | 39          | 1973199   | 1973160      | 372918          | 21706  | 5.291136907 | 303.34MHz |
| DHRYSTONE | 39          | 1385849   | 1385810      | 231368          | 45786  | 5.989635559 | 303.34MHz |
| VVADD     | 39          | 65679     | 65640        | 11072           | 2048   | 5.928468208 | 303.34MHz |
| MEDIAN    | 39          | 97509     | 97470        | 15427           | 4095   | 6.318143515 | 303.34MHz |
