from .iocl_codebase import IOCLCodebase
from .transformed_codebase import TransformedCodebase


__BUILDERS__ = {
    "iocl": IOCLCodebase(),
    "transformed": TransformedCodebase()
}


def get_client_cmd(config, i, k, run, local_exp_directory, remote_exp_directory):
    b = __BUILDERS__[config['codebase_name']]
    return b.get_client_cmd(config, i, k, run, local_exp_directory, remote_exp_directory)


def get_replica_cmd(config, instance_idx, shard_idx, replica_idx, run, local_exp_directory, remote_exp_directory):
    b = __BUILDERS__[config['codebase_name']]
    return b.get_replica_cmd(config, instance_idx, shard_idx, replica_idx, run, local_exp_directory, remote_exp_directory)


def prepare_local_exp_directory(config, config_file):
    b = __BUILDERS__[config['codebase_name']]
    return b.prepare_local_exp_directory(config, config_file)


def prepare_remote_server_codebase(config, server_host, local_exp_directory, remote_out_directory):
    b = __BUILDERS__[config['codebase_name']]
    return b.prepare_remote_server_codebase(config, server_host, local_exp_directory, remote_out_directory)


def setup_nodes(config):
    b = __BUILDERS__[config['codebase_name']]
    return b.setup_nodes(config)
