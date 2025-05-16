# SPDX-License-Identifier: GPL-2.0-only

import argparse
import os
import random
import shutil
import subprocess
from itertools import product

BASE_DIR = '../'
TEST_DIR = os.path.join(BASE_DIR, 'test/generated_tf')
DEFAULT_SUITABLE_FILE_SIZES = [32, 36, 48, 64, 128, 256, 512, 1024, 2048, 4096]

class File:
    """Represents a test file with associated operations."""
    def __init__(self, name, size_kb, directory=TEST_DIR):
        self.name = name
        self.size_kb = size_kb
        self.directory = directory

    @property
    def path(self):
        return os.path.join(self.directory, self.name)

    def create(self):
        os.makedirs(self.directory, exist_ok=True)
        with open(self.path, 'wb') as f:
            f.write(os.urandom(self.size_kb * 1024))
        print(f"Created test file: {self.path} ({self.size_kb} KB)")

    def compare(self, other_file):
        chunk_num = 0
        with open(self.path, 'rb') as f1, open(other_file.path, 'rb') as f2:
            while True:
                chunk_num += 1;
                chunk1 = f1.read(4096)
                chunk2 = f2.read(4096)
                if chunk1 != chunk2:
                    print(f"Mismatch found between {self.path} and {other_file.path}.")
                    return chunk_num
                if not chunk1:
                    break
        print('\033[1m' + f"Files {self.path} and {other_file.path} are identical." + '\033[0m')
        return 0

    def delete(self):
        if os.path.exists(self.path):
            os.unlink(self.path)
            print(f"Deleted file: {self.path}")

class Test:
    """Represents a test operation and its parameters."""
    def __init__(self, vbd_name, block_size_kb=0, mode="seq"):
        self.vbd_name = vbd_name
        self.block_size_kb = block_size_kb
        self.mode = mode
        self.test_count = 0
        self.errors = []

    def get_even_divisors(self, n):
        """The range of divisors is limited ( <= 8)due scatterlist kernel bug on a VM (6.8.0-50 kernel version)
        BTW: this problem appears only in this type of testing, the inconsequent (+ fio) passes.
        TODO: test on a x86-64 machine! (without virtualisation)"""
        divisors = set()
        for i in range(2, int(n**0.5) + 1):
            if n % i == 0:
                if i % 2 == 0 and i <= 8 and i >= 4:
                    divisors.add(i)
                if (n // i) % 2 == 0 and (n // i) <= 8 and (n // 1) >= 4:
                    divisors.add(n // i)
        return divisors

    def shrink_list(self, prod, n):
        prod = list(prod)
        while len(prod) != n:
            random_index = random.randint(0, len(prod) - 1)
            prod.pop(random_index)
        return prod

    def process_file(self, input_file, output_file):
        file_size = os.path.getsize(input_file.path)
        block_sizes = [self.block_size_kb] if self.block_size_kb > 0 else self.get_even_divisors(file_size // 1024)
        print(block_sizes)
        if self.mode == "seq":
            seek_offset = 0
            skip_offset = 0
            op_num = 4
            prod = self.shrink_list(product(block_sizes, repeat=2), op_num)
            print(prod)
            for write_bs, read_bs in prod:
                print()
                print('\033[1m' + f"=== OPERATION N.{self.test_count + 1} ===" + '\033[0m')

                write_count = file_size // (write_bs * 1024) // op_num
                read_count = file_size // (read_bs * 1024) // op_num

                if write_count + read_count == 0:
                    continue

                self.run_dd_command(f"dd if={input_file.path} of=/dev/{self.vbd_name} oflag=direct bs={write_bs}k count={write_count} seek={seek_offset // write_bs // 1024} skip={seek_offset // write_bs // 1024}")
                self.run_dd_command(f"dd if=/dev/{self.vbd_name} of={output_file.path} iflag=direct bs={read_bs}k count={read_count} skip={skip_offset // read_bs // 1024} seek={skip_offset // read_bs // 1024}")

                self.test_count += 1

                seek_offset += write_count * write_bs * 1024
                skip_offset += read_count * read_bs * 1024

            result = input_file.compare(output_file)
            if result != 0:
                self.errors.append((result, (write_bs, read_bs)))
            self.reinit_driver()

        elif self.mode == "inc":
            for write_bs, read_bs in product(block_sizes, repeat=2):
                write_count = file_size // (write_bs * 1024)
                read_count = file_size // (read_bs * 1024)

                self.run_dd_command(f"dd if={input_file.path} of=/dev/{self.vbd_name} oflag=direct bs={write_bs}k count={write_count}")
                self.run_dd_command(f"dd if=/dev/{self.vbd_name} of={output_file.path} iflag=direct bs={read_bs}k count={read_count}")
                self.test_count += 1

                result = input_file.compare(output_file)
                if result != 0:
                    self.errors.append((write_bs, read_bs))
                self.reinit_driver()
        else:
            print(f"Mode {self.mode} isn't supported. Check -h for information about available parameters.")


    @staticmethod
    def run_dd_command(command):
        try:
            subprocess.run(command, shell=True, check=True, text=True)
            print(f"Executed command: {command}")
        except subprocess.CalledProcessError as e:
            print(f"Error executing command: {e}")

    def reinit_driver(self):
        try:
            subprocess.run(f"make exit", shell=True, check=True, text=True)
            subprocess.run(f"make init_no_recompile", shell=True, check=True, text=True)
        except subprocess.CalledProcessError as e:
            print(f"Error reinitializing the driver: {e}")

class TestManager:
    """Manages the overall test flow."""
    def __init__(self, args):
        self.args = args
        self.tests = []

    def run(self):
        if self.args.clear:
            shutil.rmtree(TEST_DIR, ignore_errors=True)
            print("Test directory cleared.")
        else:
            file_size_kb = self.args.file_size_kb if self.args.file_size_kb > 0 else random.choice(DEFAULT_SUITABLE_FILE_SIZES)
            vbd_name = self.args.vbd_name

            for i in range(self.args.num_files):
                input_file = File(f"in_tf_{i + 1}_{file_size_kb}KB.txt", file_size_kb)
                output_file = File(f"out_tf_{i + 1}_{file_size_kb}KB.txt", file_size_kb)
                input_file.create()

                test = Test(vbd_name, self.args.block_size_kb, self.args.mode)
                test.process_file(input_file, output_file)

                input_file.delete()
                output_file.delete()

                self.tests.append(test)

            if any(test.errors for test in self.tests):
                print("Errors encountered in tests:", [(test.vbd_name, test.errors) for test in self.tests])
            else:
                print('\033[1m' + f"All tests passed successfully!" + '\033[0m')

def main():
    parser = argparse.ArgumentParser(description="Run tests on virtual block device using 'dd'.")
    parser.add_argument('--vbd_name', '-vbd', type=str, default="/dev/lsvbd1", help="Name of the virtual block device")
    parser.add_argument('--num_files', '-n', type=int, default=5, help="Number of test files to create")
    parser.add_argument('--file_size_kb', '-fs', type=int, default=1024, help='Size of each test file in KB. \n '
                        'Set to 0 for a random choice of 1 file size. \n '
                        'Set to -1 for running all file sizes.')
    parser.add_argument('--block_size_kb', '-bs', type=int, default=0, help='Block size for dd command in KB. \nSet to 0 for automatic selection.')
    parser.add_argument('--clear', '-c', action='store_true', help="Clear the test directory")
    parser.add_argument('--mode', '-m', type=str, default="seq", help='Defines the mode to run. \nSet to "seq" for sequential operation in one file. \nSet "inc" for inconsecutive operations, i.e. 1 operation -> 1 file')
    args = parser.parse_args()

    manager = TestManager(args)
    manager.run()

if __name__ == "__main__":
    main()
