"""Apply the same flash gate to every board script and direct idf.py use."""
from pathlib import Path
import sys


def action_extensions(base_actions, project_path):
    # Import at execution time: a missing checker must fail the action, not cause
    # IDF to silently skip loading this extension.
    def wrap(original):
        def checked(action, ctx, args, **kwargs):
            from idf_py_actions.tools import get_default_serial_port, run_target, ensure_build_directory
            from idf_py_actions.errors import FatalError
            sys.path.insert(0, str(Path(project_path) / 'scripts/eidolon'))
            import partition_contract
            ensure_build_directory(args, ctx.info_name)
            run_target('all', args)
            build = Path(args.build_dir)
            if partition_contract.build_config(build).get('CONFIG_EIDOLON_HUB_MODE') != 'y':
                return original(action, ctx, args, **kwargs)
            if action.startswith('encrypted-'):
                raise FatalError('Eidolon encrypted flashing needs an explicit encrypted-layout verification flow')
            if kwargs.get('extra_args'):
                raise FatalError('Eidolon verified flashing does not accept extra esptool write arguments')
            args.port = args.port or get_default_serial_port()
            full = action == 'flash'
            try:
                app_partition = (partition_contract.application_partition(build)
                                 if action in ('flash', 'app-flash') else None)
                partition_contract.check_device(build, args.port, allow_blank=full)
                original(action, ctx, args, **kwargs)
                partition_contract.check_device(build, args.port, after=True)
                if app_partition is not None:
                    partition_contract.activate_application(build, args.port, app_partition)
            except Exception as error:
                raise FatalError(f'Eidolon flash verification failed: {error}') from error
        return checked

    for name in ('flash', 'app-flash', 'partition-table-flash', 'bootloader-flash',
                 'encrypted-flash', 'encrypted-app-flash'):
        entry = base_actions['actions'][name]
        entry['callback'] = wrap(entry['callback'])
    return {}
