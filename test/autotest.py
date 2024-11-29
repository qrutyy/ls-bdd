
import argparse
import os
import random
import shutil
import subprocess
from itertools import product

BASE_DIR = '../'
TEST_DIR = os.path.join(BASE_DIR, 'test/generated_tf')
DEFAULT_SUITABLE_FILE_SIZES = [32, 36, 48, 64, 128, 256, 512, 1024, 2048, 4096]

test_count = 0
errors = []

def get_even_divisors(n):
    divisors = set()
    for i in range(2, int(n**0.5) + 1):
        if n % i == 0:
            if i % 2 == 0:
                divisors.add(i)
            if (n // i) % 2 == 0:
                divisors.add(n // i)
    return divisors

def run_make_commands(commands):
    try:
        for cmd in commands:
            subprocess.run(['make', cmd], cwd=BASE_DIR, check=True, text=True)
        print("Driver prepared successfully.")
    except subprocess.CalledProcessError as e:
        print(f"Error during driver preparation: {e}")

def clean_directory(directory):
    if not os.path.exists(directory):
        print(f"Directory {directory} does not exist.")
        return
    for item in os.listdir(directory):
        item_path = os.path.join(directory, item)
        try:
            if os.path.isfile(item_path) or os.path.islink(item_path):
                os.unlink(item_path)
            elif os.path.isdir(item_path):
                shutil.rmtree(item_path)
        except Exception as e:
            print(f"Failed to delete {item_path}. Reason: {e}")

def create_test_files(num_files, file_size_kb, output_dir=TEST_DIR):
    os.makedirs(output_dir, exist_ok=True)
    for i in range(num_files):
        input_file = os.path.join(output_dir, f"in_tf_{i + 1}_{file_size_kb}KB.txt")
        with open(input_file, 'wb') as f:
            f.write(os.urandom(file_size_kb * 1024))
        print(f"Created test file: {input_file} ({file_size_kb} KB)")

def compare_files(file1, file2):
    with open(file1, 'rb') as f1, open(file2, 'rb') as f2:
        while True:
            chunk1 = f1.read(4096)
            chunk2 = f2.read(4096)
            if chunk1 != chunk2:
                print(f"Mismatch found between {file1} and {file2}.")
                return False
            if not chunk1:
                break
    print(f"Files {file1} and {file2} are identical.")
    return True

def run_dd_command(command):
    try:
        subprocess.run(command, shell=True, check=True, text=True)
        print(f"Executed command: {command}")
    except subprocess.CalledProcessError as e:
        print(f"Error executing command: {e}")

def process_test_file(vbd_name, input_file, output_file, block_sizes):
    file_size = os.path.getsize(input_file)
    for write_bs, read_bs in product(block_sizes, repeat=2):
        write_count = file_size // (write_bs * 1024)
        read_count = file_size // (read_bs * 1024)

        run_dd_command(f"dd if={input_file} of=/dev/{vbd_name} oflag=direct bs={write_bs}k count={write_count}")
        run_dd_command(f"dd if=/dev/{vbd_name} of={output_file} iflag=direct bs={read_bs}k count={read_count}")

        if not compare_files(input_file, output_file):
            errors.append((write_bs, read_bs))

def run_tests(vbd_name, num_files, file_size_kb, block_size_kb):
    global test_count
    create_test_files(num_files, file_size_kb, TEST_DIR)
    block_sizes = [block_size_kb] if block_size_kb > 0 else get_even_divisors(file_size_kb)

<<<<<<< HEAD
    for i in range(1, num_files + 1):
        input_file = os.path.join(TEST_DIR, f"in_tf_{i}_{file_size_kb}KB.txt")
        output_file = os.path.join(TEST_DIR, f"out_tf_{i}_{file_size_kb}KB.txt")
        print(input_file)

        if block_size_kb == 0:
            rw_pairs = list(product(get_odd_divs(file_size_kb), repeat=2))
            for rw_bs in rw_pairs:
                input_size = os.path.getsize(input_file)
                run_dd_write_command(input_file, rw_bs[0], input_size // (rw_bs[0] * 1024))
                run_dd_read_command(output_file, rw_bs[1], input_size // (rw_bs[1] * 1024))
                print(f"Completed processing file: {input_file}")
                test_count += 1
            
                compare_files(input_file, output_file, rw_bs, [input_size // (rw_bs[0] * 1024), input_size // (rw_bs[1] * 1024)])
        else:
            count = os.path.getsize(input_file) // (block_size_kb * 1024)
            run_dd_read_command(input_file, block_size_kb, int(count))
            run_dd_write_command(output_file, block_size_kb, int(count))
            
            compare_files(input_file, output_file, list(block_size_kb), list(count))
       
def proceed_run(num_files, file_size_kb, block_size_kb):
#     prepare_driver()
    clean_dir(TEST_DIR)

    if file_size_kb == 0:
        file_size_kb = random.choice(DEF_SUITABLE_FB)
        run_test_files(num_files, file_size_kb, block_size_kb)
    elif file_size_kb == -1:
        for k in DEF_SUITABLE_FB:
            run_test_files(num_files, k, block_size_kb)
    else:
        run_test_files(num_files, file_size_kb, block_size_kb)

    if len(errors) != 0:
        print(errors)
    else:
        print("\n\033[1mAll files are identical\033[0m")

    print(f"\n\033[1mTest passed: {test_count}, Failed: {int(len(errors) / 2)}\n\033[o \n")
    clean_dir(TEST_DIR)
=======
    for i in range(num_files):
        input_file = os.path.join(TEST_DIR, f"in_tf_{i + 1}_{file_size_kb}KB.txt")
        output_file = os.path.join(TEST_DIR, f"out_tf_{i + 1}_{file_size_kb}KB.txt")
        process_test_file(vbd_name, input_file, output_file, block_sizes)
        test_count += 1
>>>>>>> aa59988 (test: add support for different vdb names)


def main():
    parser = argparse.ArgumentParser(description="Run tests on virtual block device using 'dd'.")
    parser.add_argument('--vbd_name', '-vbd', type=str, default="/dev/lsvbd1", help="Name of the virtual block device")
    parser.add_argument('--num_files', '-n', type=int, default=5, help="Number of test files to create")
    parser.add_argument('--file_size_kb', '-fs', type=int, default=1024, help="Size of each test file in KB")
    parser.add_argument('--block_size_kb', '-bs', type=int, default=0, help="Block size in KB for 'dd' operations")
    parser.add_argument('--clear', action='store_true', help="Clear the test directory and exit")
    args = parser.parse_args()

    if args.clear:
        clean_directory(TEST_DIR)
    else:
        run_make_commands(['clean', '', 'ins', 'set'])
        clean_directory(TEST_DIR)
        file_size_kb = args.file_size_kb if args.file_size_kb > 0 else random.choice(DEFAULT_SUITABLE_FILE_SIZES)
        run_tests(args.vbd_name, args.num_files, file_size_kb, args.block_size_kb)

        if errors:
            print("Errors encountered in the following tests:", errors)
        else:
            print("All tests passed successfully!")
        clean_directory(TEST_DIR)

if __name__ == "__main__":
    main()
