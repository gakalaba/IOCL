from utils.git_util import compile_make

def main():
    config = {
        "make_env" : {},
        "make_clean" : False,
        "src_directory" : "/users/akalaba/IOCL/src",
        "make_collect_bins" : ["replication/iocl_craq/tests/iocl_craq-test"]
    }
    compile_make(config)

if __name__ == "__main__":
    main()